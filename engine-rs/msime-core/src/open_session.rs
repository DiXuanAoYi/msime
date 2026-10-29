//! 按目录布局直接建出一个 `Session`，对应 `msime_capi.cpp` 里 `Session(SessionOptions)` 那一步：
//! 资源目录里找语言模型、日语词典、辅助码表，词库在 `dictionaries`，用户词库在 `user_data`。
//!
//! 语言模型进程里按路径只载入一次（C++ 的 `ngram::shared_language_model`），会话借用它，所以这里
//! 把载入的模型泄漏成 `'static`：同一路径不会泄漏第二份。

use crate::helpcode::{self, Keymap};
use crate::ime_session::ImeSession;
use crate::input_session::{FrequencyAdjustmentOptions, InputSession};
use crate::kenlm::KenLm;
use crate::nine_key_session::NineKeySession;
use crate::pinyin_provider::PinyinProvider;
use crate::provider::ProviderRegistry;
use crate::request::SchemeType;
use crate::runtime_paths::RuntimeRoots;
use crate::session::{Session, SessionOptions};
use crate::shuangpin::profile;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

#[derive(Debug, Clone)]
pub struct OpenOptions {
    pub roots: RuntimeRoots,
    pub scheme: SchemeType,
    pub shuangpin_profile: String,
    pub helpcode_schema: String,
    pub autocorrect_types: u32,
    pub helpcode: bool,
    pub chinese_punctuation: bool,
    pub learning: bool,
    pub fuzzy_pinyin: u32,
    pub frequency: FrequencyAdjustmentOptions,
}

impl OpenOptions {
    /// 和 C++ `SessionOptions` 的默认值一致：小鹤双拼、蓝田辅助码、中文标点、学习开、不纠错、不调频。
    pub fn new(roots: RuntimeRoots) -> Self {
        Self {
            roots,
            scheme: SchemeType::Quanpin,
            shuangpin_profile: "xiaohe".to_string(),
            helpcode_schema: "lantian".to_string(),
            autocorrect_types: 0,
            helpcode: true,
            chinese_punctuation: true,
            learning: true,
            fuzzy_pinyin: 0,
            frequency: FrequencyAdjustmentOptions::default(),
        }
    }
}

fn shared_language_model(path: &Path) -> Option<&'static KenLm> {
    static MODELS: Mutex<Option<HashMap<PathBuf, Option<&'static KenLm>>>> = Mutex::new(None);
    let mut guard = MODELS.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    let models = guard.get_or_insert_with(HashMap::new);
    *models.entry(path.to_path_buf()).or_insert_with(|| KenLm::open(path).ok().map(|model| &*Box::leak(Box::new(model))))
}

/// 内置方案按表找文件，`custom:<名字>` 读 `helpcodes/custom/<名字>.txt`，其余不认识。
pub fn load_helpcode_keymap(resources: &Path, schema: &str) -> Result<Keymap, String> {
    let file = if let Some((_, relative)) = helpcode::BUILTIN_SCHEMAS.iter().find(|(name, _)| *name == schema) {
        resources.join(relative)
    } else if let Some(stem) = schema.strip_prefix("custom:").filter(|stem| !stem.is_empty() && !stem.starts_with('.') && !stem.contains(['/', '\\', ':', '*', '?', '"', '<', '>', '|'])) {
        resources.join("helpcodes").join("custom").join(format!("{stem}.txt"))
    } else {
        return Err("Unknown helpcode schema".to_string());
    };
    // 文件读不到当空码表，和 C++ 的 ifstream 一样。
    Ok(helpcode::parse_keymap(&std::fs::read_to_string(file).unwrap_or_default()))
}

pub fn open(options: &OpenOptions) -> Result<Session<'static>, String> {
    let roots = options.roots.clone();
    let main_db = roots.dictionaries.join("msime.db");
    let journal = roots.user_data.join("msime_user.db").to_string_lossy().into_owned();
    let model = shared_language_model(&roots.resources.join("sc.lm"));
    let keymap = load_helpcode_keymap(&roots.resources, &options.helpcode_schema)?;
    let profile = profile(&options.shuangpin_profile);
    let pinyin = PinyinProvider::open(profile, &main_db, &journal, keymap.clone(), model.map(|m| m as &dyn crate::lattice::LanguageModel));
    let registry = ProviderRegistry::new(pinyin, &main_db, &journal, &roots.resources.join("dict_japanese.dat"));
    let engine = ImeSession::new(options.scheme, profile, registry);
    let mut input = InputSession::new(engine, roots.clone(), profile);
    input.set_helpcode_keymap(keymap);
    let nine_key = NineKeySession::new(roots, options.learning, options.frequency, options.fuzzy_pinyin, model.map(|m| m as &dyn crate::lattice::LanguageModel));
    let session_options = SessionOptions {
        autocorrect_types: options.autocorrect_types,
        helpcode: options.helpcode,
        chinese_punctuation: options.chinese_punctuation,
        learning: options.learning,
        fuzzy_pinyin: options.fuzzy_pinyin,
        frequency: options.frequency,
        local_modes: Default::default(),
        english: Default::default(),
        expressive: Default::default(),
        wubi: Default::default(),
    };
    Session::new(input, nine_key, &session_options)
}
