//! 候选查询：本地模式（Unicode、日期时间、快捷短语、表情、颜文字、超级简拼、临时英文、临时日语）的查询，
//! 以及把英文、表情、颜文字候选混进普通候选列表的规则。对应 `engine/core/candidate_queries.cpp`。
//!
//! 这一层不碰按键和组合串，只管资源查询与合并。资源路径跟 C++ 一样：词库走“词库目录”，表情、颜文字库与翻译侧车文件走“资源目录”。
//! C++ 在没传时钟时读系统本地时间（`current_local_date_time`）；这里要求调用方给时钟，系统时钟留给宿主接。

use crate::datetime::{self, DateTime};
use crate::english::EnglishDictionary;
use crate::lattice::{Item, Source};
use crate::local;
use crate::request::SchemeType;
use crate::runtime_paths::RuntimeRoots;
use crate::shuangpin::Profile;
use crate::vmode;
use std::collections::HashSet;
use std::path::{Path, PathBuf};

/// 资源文件名（`contracts/assets`）。
const MAIN_DICTIONARY: &str = "msime.db";
const ENGLISH_DICTIONARY: &str = "english.db";
const OTHER_DICTIONARY: &str = "others.db";
const TRANSLATIONS: &str = "custom_translations.txt";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LocalInputMode {
    #[default]
    None,
    Unicode,
    DateTime,
    QuickPhrase,
    Emoji,
    Kaomoji,
    SuperJianpin,
    TemporaryEnglish,
    TemporaryJapanese,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EnglishInputOptions {
    pub mixed_candidates: bool,
    pub minimum_prefix: usize,
}

impl Default for EnglishInputOptions {
    fn default() -> Self {
        Self { mixed_candidates: false, minimum_prefix: 2 }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct MixedExpressiveOptions {
    pub emoji_candidates: bool,
    pub kaomoji_candidates: bool,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct LocalQueryResult {
    pub candidates: Vec<Item>,
    pub diagnostic: Option<String>,
}

/// 本地模式候选转成会话的候选：方案默认全拼，其余字段取默认。
fn convert(item: local::Item) -> Item {
    Item {
        pinyin: item.pinyin,
        canonical_pinyin: item.canonical_pinyin,
        word: item.word,
        weight: item.weight,
        source: item.source,
        sentence_association: false,
        input_domains: Vec::new(),
        fuzzy: false,
        corrected_from: String::new(),
        scheme: SchemeType::Quanpin,
        fixed_position: item.fixed_position,
    }
}

fn convert_all(items: Vec<local::Item>) -> Vec<Item> {
    items.into_iter().map(convert).collect()
}

fn from_query(result: local::QueryResult) -> LocalQueryResult {
    LocalQueryResult { candidates: convert_all(result.items), diagnostic: result.diagnostic }
}

/// 与 C++ 的 `RuntimePaths::resource` / `dictionary` 一样：根为空就给空路径。
fn join(root: &Path, name: &str) -> PathBuf {
    if root.as_os_str().is_empty() {
        PathBuf::new()
    } else {
        root.join(name)
    }
}

/// 临时英文模式候选里要剔除的：与输入逐字（忽略大小写）相同的词。
fn equals_ignore_ascii_case(left: &str, right: &str) -> bool {
    left.len() == right.len() && left.bytes().zip(right.bytes()).all(|(a, b)| a.to_ascii_lowercase() == b.to_ascii_lowercase())
}

pub struct CandidateQueries {
    roots: RuntimeRoots,
    profile: &'static Profile,
    english: Option<EnglishDictionary>,
}

impl CandidateQueries {
    pub fn new(roots: RuntimeRoots, profile: &'static Profile) -> Self {
        Self { roots, profile, english: None }
    }

    /// 英文词典，第一次用到时才打开。
    pub fn english_dictionary(&mut self) -> &mut EnglishDictionary {
        if self.english.is_none() {
            let database = join(&self.roots.dictionaries, ENGLISH_DICTIONARY);
            let translations = join(&self.roots.resources, TRANSLATIONS);
            self.english = Some(EnglishDictionary::open(&database, Some(&translations)));
        }
        self.english.as_mut().expect("上面刚建好")
    }

    /// 本地模式的候选。`preedit` 的第一个字符是模式的触发字母，其余是模式里敲的码；
    /// `engine_candidates` 只有临时日语用（它直接用引擎的候选）。
    pub fn local(
        &mut self,
        mode: LocalInputMode,
        preedit: &str,
        scheme: SchemeType,
        clock: &dyn Fn() -> DateTime,
        engine_candidates: &[Item],
    ) -> LocalQueryResult {
        // C++ 的 `substr(1)`：触发字母是 ASCII，所以按字节切；空串没有触发字母，当作没有码。
        let code = preedit.get(1..).unwrap_or("");
        let shuangpin = scheme == SchemeType::Shuangpin;
        let main = join(&self.roots.dictionaries, MAIN_DICTIONARY);
        let other = join(&self.roots.resources, OTHER_DICTIONARY);
        match mode {
            LocalInputMode::Unicode => LocalQueryResult { candidates: convert_all(vmode::query_unicode(code, 8)), diagnostic: None },
            LocalInputMode::DateTime => {
                let now = clock();
                LocalQueryResult { candidates: convert_all(datetime::query_date_time(code, &now, 17)), diagnostic: None }
            }
            LocalInputMode::QuickPhrase => from_query(local::query_quick_phrases(code, &main, 100)),
            LocalInputMode::Emoji => from_query(local::query_emoji(code, shuangpin, &other, 10, &self.profile)),
            LocalInputMode::Kaomoji => from_query(local::query_kaomoji(code, shuangpin, &other, 10, &self.profile)),
            LocalInputMode::SuperJianpin => {
                let limit = if code.len() == 1 { 24 } else { 100 };
                from_query(local::query_jianpin(code, shuangpin, &main, limit, &self.profile))
            }
            LocalInputMode::TemporaryEnglish => {
                if code.is_empty() {
                    return LocalQueryResult::default();
                }
                let mut candidates = vec![Item {
                    pinyin: String::new(),
                    canonical_pinyin: String::new(),
                    word: code.to_string(),
                    weight: 0,
                    source: Source::Generated,
                    sentence_association: false,
                    input_domains: Vec::new(),
                    fuzzy: false,
                    corrected_from: String::new(),
                    scheme: SchemeType::Quanpin,
                    fixed_position: 0,
                }];
                let prefix = code.to_ascii_lowercase();
                let completions = self.english_dictionary().query_prefix(&prefix, 1000);
                candidates.extend(convert_all(completions).into_iter().filter(|candidate| !equals_ignore_ascii_case(&candidate.word, code)));
                LocalQueryResult { candidates, diagnostic: None }
            }
            LocalInputMode::TemporaryJapanese => LocalQueryResult { candidates: engine_candidates.to_vec(), diagnostic: None },
            LocalInputMode::None => LocalQueryResult::default(),
        }
    }

    /// 把英文、表情、颜文字候选混进普通候选：各自最靠前的一条插在优先槽位（云候选之后、AI 候选之后更靠后），
    /// 其余追加在末尾；与已有候选同文字的不重复加。
    #[allow(clippy::too_many_arguments)]
    pub fn mixed(
        &mut self,
        mut candidates: Vec<Item>,
        prefix: &str,
        scheme: SchemeType,
        english_options: EnglishInputOptions,
        expressive_options: MixedExpressiveOptions,
        dedicated_english: bool,
        local_mode: LocalInputMode,
    ) -> Vec<Item> {
        if (!english_options.mixed_candidates && !expressive_options.emoji_candidates && !expressive_options.kaomoji_candidates)
            || dedicated_english
            || local_mode != LocalInputMode::None
            || (scheme != SchemeType::Quanpin && scheme != SchemeType::Shuangpin)
        {
            return candidates;
        }
        if prefix.is_empty() {
            return candidates;
        }

        let mut seen: HashSet<String> = candidates.iter().map(|candidate| candidate.word.clone()).collect();
        let mut collect_unique = |items: Vec<Item>| -> Vec<Item> { items.into_iter().filter(|item| seen.insert(item.word.clone())).collect() };

        let shuangpin = scheme == SchemeType::Shuangpin;
        let other = join(&self.roots.resources, OTHER_DICTIONARY);

        let mut english_candidates = Vec::new();
        let lower_ascii_prefix = prefix.bytes().all(|b| b.is_ascii_lowercase());
        if english_options.mixed_candidates && prefix.len() >= english_options.minimum_prefix && lower_ascii_prefix {
            let found = convert_all(self.english_dictionary().query_prefix(prefix, 5));
            english_candidates = collect_unique(found);
        }

        let mut emoji_candidates = Vec::new();
        if expressive_options.emoji_candidates && prefix.len() >= 2 {
            let found = convert_all(local::query_emoji(prefix, shuangpin, &other, 3, &self.profile).items);
            emoji_candidates = collect_unique(found);
        }

        let mut kaomoji_candidates = Vec::new();
        if expressive_options.kaomoji_candidates && prefix.len() >= 2 {
            let found = convert_all(local::query_kaomoji(prefix, shuangpin, &other, 3, &self.profile).items);
            kaomoji_candidates = collect_unique(found);
        }

        let has_source = |source: Source| candidates.iter().any(|candidate| candidate.source == source);
        let mut priority_slot = candidates.len().min(1);
        if has_source(Source::CloudSuggestion) {
            priority_slot = candidates.len().min(2);
        }
        if has_source(Source::AiSuggestion) {
            priority_slot = candidates.len().min(3);
        }
        for source in [&mut english_candidates, &mut emoji_candidates, &mut kaomoji_candidates] {
            if source.is_empty() {
                continue;
            }
            candidates.insert(priority_slot, source.remove(0));
            priority_slot += 1;
        }
        for source in [english_candidates, emoji_candidates, kaomoji_candidates] {
            candidates.extend(source);
        }
        candidates
    }
}
