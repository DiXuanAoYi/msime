//! 整句来源共用的资源：Google 解码器、两个神经模型、万象语法库，和神经重排的后台线程。
//! 对应 C++ 的 `neural/rescore_worker.*`，加上全拼、双拼词库各自持有的模型句柄（C++ 里是进程内按路径共享的）。
//!
//! 神经重排给十二条候选打一次分要几十毫秒，不能长在按键路径上：查询照旧同步返回词格的静态顺序，同时把这一批候选
//! 交给后台线程；线程算完把顺序存进结果表，再叫醒调用方，调用方重查一次，这次就能在表里查到顺序。
//!
//! - “每个模型最新的赢”：同一模型的新请求直接盖掉它旧的待办；不同模型各有一个待办位和执行槽。
//! - 正在算的那一批不再排第二份。
//! - 结果表按 (模型, 前文, 候选列表) 做键，不含静态分；装满（128 条）就整张丢掉。

use crate::gram::GramDb;
use crate::google_pinyin::PinyinDecoder;
use crate::lattice::Path as LatticePath;
use crate::neural::{last_characters, rerank_order, RerankOptions, SentenceModel};
use std::collections::{BTreeMap, HashMap};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};

const MAX_RESULTS: usize = 128;
const WORKER_COUNT: usize = 2;

pub type Ready = Arc<dyn Fn() + Send + Sync>;

struct Job {
    model: Arc<SentenceModel>,
    key: String,
    context: String,
    sentences: Vec<String>,
    static_scores: Vec<f64>,
    options: RerankOptions,
}

#[derive(Default)]
struct Inner {
    threads_started: bool,
    busy_workers: usize,
    stopping: bool,
    pending: BTreeMap<usize, Job>,
    active_keys: BTreeMap<usize, String>,
    done: BTreeMap<String, Vec<usize>>,
    ready: Option<Ready>,
}

struct Shared {
    inner: Mutex<Inner>,
    wake: Condvar,
    idle: Condvar,
}

fn lock(shared: &Shared) -> MutexGuard<'_, Inner> {
    shared.inner.lock().unwrap_or_else(|e| e.into_inner())
}

fn model_id(model: &Arc<SentenceModel>) -> usize {
    Arc::as_ptr(model) as usize
}

fn make_key(model: usize, context: &str, sentences: &[String]) -> String {
    // \x01 不会出现在 UTF-8 文本里，拿它当分隔符就不会把两批不同的候选拼成同一个键。
    let mut key = model.to_string();
    key.push('\x01');
    key.push_str(context);
    for sentence in sentences {
        key.push('\x01');
        key.push_str(sentence);
    }
    key
}

pub struct RescoreWorker {
    shared: Arc<Shared>,
}

impl Default for RescoreWorker {
    fn default() -> Self {
        Self::new()
    }
}

impl RescoreWorker {
    pub fn new() -> Self {
        RescoreWorker { shared: Arc::new(Shared { inner: Mutex::new(Inner::default()), wake: Condvar::new(), idle: Condvar::new() }) }
    }

    /// 把“算完了”的通知接出去，调用方据此重查。不设也行：结果照样进表，只是要等下一次按键才被取走。
    pub fn set_ready_callback(&self, ready: Option<Ready>) {
        lock(&self.shared).ready = ready;
    }

    /// 有现成结果就返回；没有就把这一批排上队并返回 `None`，调用方这一次照静态顺序走。
    pub fn order_for(&self, model: &Arc<SentenceModel>, context: &str, sentences: &[String], static_scores: &[f64], options: &RerankOptions) -> Option<Vec<usize>> {
        let id = model_id(model);
        let key = make_key(id, context, sentences);
        {
            let mut inner = lock(&self.shared);
            if inner.stopping {
                return None;
            }
            if let Some(order) = inner.done.get(&key) {
                return Some(order.clone());
            }
            if inner.active_keys.get(&id) == Some(&key) {
                // 正在算的就是这一批，算完自会进表并回调。刻意不碰 pending：那里可能已经是更新的请求。
                return None;
            }
            inner.pending.insert(
                id,
                Job { model: Arc::clone(model), key, context: context.to_string(), sentences: sentences.to_vec(), static_scores: static_scores.to_vec(), options: *options },
            );
            if !inner.threads_started {
                inner.threads_started = true;
                for _ in 0..WORKER_COUNT {
                    let shared = Arc::clone(&self.shared);
                    std::thread::spawn(move || run(shared));
                }
            }
        }
        self.shared.wake.notify_one();
        None
    }

    /// 上屏、换方案、换前文：存着的顺序对新语境没有意义了。
    pub fn clear(&self) {
        let mut inner = lock(&self.shared);
        inner.done.clear();
        inner.pending.clear();
    }

    /// 停工，并等当前这一批算完。
    pub fn shutdown(&self) {
        let mut inner = lock(&self.shared);
        inner.stopping = true;
        inner.ready = None;
        inner.pending.clear();
        self.shared.wake.notify_all();
        while inner.busy_workers != 0 {
            inner = self.shared.idle.wait(inner).unwrap_or_else(|e| e.into_inner());
        }
    }

    /// 后台没有待办也没有在算的批次（测试等结果用）。
    pub fn is_idle(&self) -> bool {
        let inner = lock(&self.shared);
        inner.pending.is_empty() && inner.busy_workers == 0
    }
}

impl Drop for RescoreWorker {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn run(shared: Arc<Shared>) {
    loop {
        let (id, job) = {
            let mut inner = lock(&shared);
            let id = loop {
                if inner.stopping {
                    return;
                }
                if let Some(id) = inner.pending.keys().copied().find(|id| !inner.active_keys.contains_key(id)) {
                    break id;
                }
                inner = shared.wake.wait(inner).unwrap_or_else(|e| e.into_inner());
            };
            let job = inner.pending.remove(&id).expect("刚找到的待办还在");
            inner.active_keys.insert(id, job.key.clone());
            inner.busy_workers += 1;
            (id, job)
        };

        // 打分在锁外做：它是这里最慢的一步，占着锁会让按键路径上的 order_for 跟着等。
        let order = catch_unwind(AssertUnwindSafe(|| rerank_order(&job.model, &job.context, &job.sentences, &job.static_scores, &job.options)));
        let ready = match order {
            Ok(order) => {
                let mut inner = lock(&shared);
                // 期间可能又来了新请求，说明用户还在打字，这一份存下来留给“他退回来”的情况。
                if inner.done.len() >= MAX_RESULTS {
                    inner.done.clear();
                }
                inner.done.entry(job.key.clone()).or_insert(order);
                inner.ready.clone()
            }
            // 打分抛了就是没有顺序；也刻意不叫回调，免得坏得稳定的模型在“重查-再排”里空转。
            Err(_) => None,
        };
        // 回调在锁外发：它会一路走到重查，重查又会调 order_for，在锁内发就是自锁。
        if let Some(ready) = ready {
            let _ = catch_unwind(AssertUnwindSafe(|| ready()));
        }

        {
            let mut inner = lock(&shared);
            inner.active_keys.remove(&id);
            inner.busy_workers -= 1;
        }
        shared.wake.notify_all();
        shared.idle.notify_all();
    }
}

/// 全拼、双拼词库共用的整句来源资源。模型路径变了或者文件不在都不会报错：拿到 `None`，对应来源静默退回。
pub struct SentenceSources {
    resources: PathBuf,
    google: Mutex<Option<Option<Arc<PinyinDecoder>>>>,
    models: Mutex<HashMap<PathBuf, Option<Arc<SentenceModel>>>>,
    grams: Mutex<HashMap<PathBuf, Option<Arc<GramDb>>>>,
    pub worker: RescoreWorker,
}

impl SentenceSources {
    /// `resources` 是资源目录（`dict_pinyin.dat`、`sentence-model.safetensors` 所在）。
    pub fn new(resources: &Path) -> Arc<Self> {
        Arc::new(SentenceSources {
            resources: resources.to_path_buf(),
            google: Mutex::new(None),
            models: Mutex::new(HashMap::new()),
            grams: Mutex::new(HashMap::new()),
            worker: RescoreWorker::new(),
        })
    }

    pub fn google(&self) -> Option<Arc<PinyinDecoder>> {
        let mut slot = self.google.lock().unwrap_or_else(|e| e.into_inner());
        slot.get_or_insert_with(|| PinyinDecoder::load_file(&self.resources.join("dict_pinyin.dat")).map(Arc::new)).clone()
    }

    /// 快速档模型，固定在资源目录。
    pub fn keyboard_model(&self) -> Option<Arc<SentenceModel>> {
        self.model(&self.resources.join("sentence-model.safetensors"))
    }

    /// 高精度档模型：选项里给了路径就用它，空路径回退到资源目录里的旧位置。
    pub fn desktop_model(&self, option_path: &str) -> Option<Arc<SentenceModel>> {
        if option_path.is_empty() {
            self.model(&self.resources.join("sentence-model-desktop.safetensors"))
        } else {
            self.model(Path::new(option_path))
        }
    }

    fn model(&self, path: &Path) -> Option<Arc<SentenceModel>> {
        let mut models = self.models.lock().unwrap_or_else(|e| e.into_inner());
        models.entry(path.to_path_buf()).or_insert_with(|| SentenceModel::load_file(path).map(Arc::new)).clone()
    }

    /// 万象语法库；路径为空或打不开返回 `None`。
    pub fn gram(&self, path: &str) -> Option<Arc<GramDb>> {
        if path.is_empty() {
            return None;
        }
        let mut grams = self.grams.lock().unwrap_or_else(|e| e.into_inner());
        grams.entry(PathBuf::from(path)).or_insert_with(|| GramDb::open(Path::new(path)).ok().map(Arc::new)).clone()
    }
}

/// 词格 n-best 的神经重排器：前文先裁到模型真正会看的那几十个字（结果表以“前文 + 候选句”为键）；
/// 后台还没算出来或模型弃权时返回 false，词格的顺序原样留着。
pub fn neural_reranker<'a>(sources: &'a SentenceSources, model: &'a Arc<SentenceModel>, context: &str) -> impl Fn(&mut Vec<LatticePath>) -> bool + 'a {
    let options = RerankOptions::default();
    let context = last_characters(context, options.context_chars).to_string();
    move |paths: &mut Vec<LatticePath>| {
        if paths.len() < 2 {
            return false;
        }
        let sentences: Vec<String> = paths.iter().map(|p| p.sentence.clone()).collect();
        let scores: Vec<f64> = paths.iter().map(|p| p.log_prob).collect();
        match sources.worker.order_for(model, &context, &sentences, &scores, &options) {
            Some(order) if order.len() == paths.len() => {
                let reordered: Vec<LatticePath> = order.into_iter().map(|i| paths[i].clone()).collect();
                *paths = reordered;
                true
            }
            _ => false,
        }
    }
}
