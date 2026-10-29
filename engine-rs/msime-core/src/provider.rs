//! 候选提供者的登记处：按方案把查询、调频、删词、动态候选缓存分给拼音、五笔、日语三个提供者，
//! 对应 `engine/providers/provider_registry.cpp`、`wubi_candidate_provider.cpp`、`japanese_candidate_provider.cpp`。
//!
//! 五笔候选显式标成 `Wubi`（混输里它们与拼音候选共存）；日语候选不标，默认 `Quanpin`，与 C++ 一致。

use crate::cache::Cache;
use crate::japanese::{self, Provider as JapaneseProvider};
use crate::lattice::{Item, Source};
use crate::pinyin_provider::PinyinProvider;
use crate::request::{QueryRequest, SchemeType};
use crate::user_dictionary::{self, DictionaryKind};
use crate::wubi::{self, Dictionary as WubiDictionary};
use std::path::{Path, PathBuf};

/// 写库类入口的“成功”：五笔、日语没有的操作也返回它。
const NO_MUTATION: i32 = 0;
const MUTATION_FAILED: i32 = -1;

fn make_item(pinyin: &str, canonical: &str, word: &str, weight: i64, source: Source, scheme: SchemeType) -> Item {
    Item {
        pinyin: pinyin.to_string(),
        canonical_pinyin: canonical.to_string(),
        word: word.to_string(),
        weight,
        source,
        sentence_association: false,
        input_domains: Vec::new(),
        fuzzy: false,
        corrected_from: String::new(),
        scheme,
        fixed_position: 0,
    }
}

pub struct ProviderRegistry<'a> {
    pub pinyin: PinyinProvider<'a>,
    wubi: Option<WubiDictionary>,
    db_path: PathBuf,
    journal_path: String,
    japanese: Option<JapaneseProvider>,
    /// 日语的云候选缓存（C++ 的 `dynamic_candidates_`，容量 128），键是输入的罗马字。
    japanese_dynamic: Cache<Vec<japanese::Candidate>>,
}

impl<'a> ProviderRegistry<'a> {
    /// `japanese_model` 是日语词典模型；打不开就没有日语候选。
    pub fn new(pinyin: PinyinProvider<'a>, db_path: &Path, journal_path: &str, japanese_model: &Path) -> Self {
        ProviderRegistry {
            pinyin,
            wubi: WubiDictionary::open(db_path).ok(),
            db_path: db_path.to_path_buf(),
            journal_path: journal_path.to_string(),
            japanese: JapaneseProvider::open(db_path, japanese_model).ok(),
            japanese_dynamic: Cache::new(128),
        }
    }

    pub fn query(&mut self, request: &QueryRequest) -> Vec<Item> {
        match request.scheme {
            SchemeType::Quanpin | SchemeType::Shuangpin => self.pinyin.query(request),
            SchemeType::Wubi => self.query_wubi(request),
            SchemeType::JapaneseRomaji => self.query_japanese(request),
        }
    }

    fn query_wubi(&self, request: &QueryRequest) -> Vec<Item> {
        if !request.valid || request.normalized_input.is_empty() {
            return Vec::new();
        }
        let Some(dictionary) = &self.wubi else { return Vec::new() };
        let wubi_request = wubi::Request {
            code: request.normalized_input.clone(),
            key_strokes: request.key_strokes.iter().map(|s| wubi::KeyStroke { vk: s.vk, modifiers: s.modifiers_down, wch: s.wch }).collect(),
            z_wildcard: request.wubi_z_wildcard,
            valid: request.valid,
        };
        dictionary.query(&wubi_request).into_iter().map(|c| make_item(&c.pinyin, &c.canonical_pinyin, &c.word, c.weight, c.source, SchemeType::Wubi)).collect()
    }

    fn query_japanese(&self, request: &QueryRequest) -> Vec<Item> {
        if !request.valid {
            return Vec::new();
        }
        let Some(provider) = &self.japanese else { return Vec::new() };
        let japanese_request = japanese::Request {
            raw_input: request.raw_input.clone(),
            raw_input_with_cases: request.raw_input_with_cases.clone(),
            normalized_input: request.normalized_input.clone(),
            raw_segmentation: request.raw_segmentation.clone(),
            normalized_segmentation: request.normalized_segmentation.clone(),
            segmentation: request.segmentation.clone(),
            key_strokes: request.key_strokes.iter().map(|s| wubi::KeyStroke { vk: s.vk, modifiers: s.modifiers_down, wch: s.wch }).collect(),
            valid: request.valid,
        };
        let dynamic = self.japanese_dynamic.get(&request.raw_input).unwrap_or_default();
        provider.query(&japanese_request, &dynamic).into_iter().map(|c| make_item(&c.pinyin, &c.canonical_pinyin, &c.word, c.weight, c.source, SchemeType::JapaneseRomaji)).collect()
    }

    pub fn find_candidate(&self, scheme: SchemeType, key: &str, value: &str) -> Option<Item> {
        match scheme {
            SchemeType::Quanpin | SchemeType::Shuangpin => self.pinyin.find_candidate(scheme, key, value),
            SchemeType::Wubi => None,
            SchemeType::JapaneseRomaji => {
                self.japanese.as_ref()?.find_candidate(key, value).map(|c| make_item(&c.pinyin, &c.canonical_pinyin, &c.word, c.weight, c.source, SchemeType::JapaneseRomaji))
            }
        }
    }

    pub fn expand_initial_candidates(&mut self, request: &QueryRequest, candidates: &mut Vec<Item>) -> bool {
        self.pinyin.expand_initial_candidates(request, candidates)
    }

    pub fn reset_cache(&mut self, scheme: SchemeType) {
        match scheme {
            SchemeType::Quanpin | SchemeType::Shuangpin => self.pinyin.reset_cache(),
            // 五笔的查询每次都用新连接，没有缓存可清；写库之后的查询自然读到新权重。
            SchemeType::Wubi => {}
            SchemeType::JapaneseRomaji => self.japanese_dynamic.clear(),
        }
    }

    /// 容错仲裁选中读音后的分段显示（大小写原样）；空表示当前串没有容错布局。
    pub fn fault_segmentation_with_cases(&self) -> &str {
        self.pinyin.fault_segmentation_with_cases()
    }

    /// 整句（词格 + 神经重排）只出在拼音提供者上，五笔混输查的也是它。
    pub fn reset_sentence_cache(&self) {
        self.pinyin.reset_sentence_cache();
    }

    pub fn create_word(&mut self, scheme: SchemeType, pinyin: &str, word: &str) -> i32 {
        match scheme {
            SchemeType::Quanpin | SchemeType::Shuangpin => self.pinyin.create_word(scheme, pinyin, word),
            // 在线造词不在五笔的范围里：手工造词走设置页的词典管理。
            SchemeType::Wubi | SchemeType::JapaneseRomaji => NO_MUTATION,
        }
    }

    pub fn update_weight_by_pinyin_and_word(&mut self, scheme: SchemeType, pinyin: &str, word: &str) -> i32 {
        match scheme {
            SchemeType::Quanpin | SchemeType::Shuangpin => self.pinyin.update_weight_by_pinyin_and_word(scheme, pinyin, word),
            // 新权重 = 同码组当前最高权重 + 1；主库更新与日志 upsert 同事务完成。
            SchemeType::Wubi => {
                if user_dictionary::bump_wubi_weight(&self.db_path.to_string_lossy(), &self.journal_path, pinyin, word) {
                    self.reset_cache(SchemeType::Wubi);
                    NO_MUTATION
                } else {
                    MUTATION_FAILED
                }
            }
            SchemeType::JapaneseRomaji => NO_MUTATION,
        }
    }

    pub fn delete_by_pinyin_and_word(&mut self, scheme: SchemeType, pinyin: &str, word: &str) -> i32 {
        match scheme {
            SchemeType::Quanpin | SchemeType::Shuangpin => self.pinyin.delete_by_pinyin_and_word(scheme, pinyin, word),
            SchemeType::Wubi => {
                if user_dictionary::delete_dictionary_candidate(&self.db_path.to_string_lossy(), &self.journal_path, DictionaryKind::Wubi, pinyin, word) {
                    self.reset_cache(SchemeType::Wubi);
                    NO_MUTATION
                } else {
                    MUTATION_FAILED
                }
            }
            SchemeType::JapaneseRomaji => NO_MUTATION,
        }
    }

    pub fn cache_dynamic_candidate(&mut self, scheme: SchemeType, code: &str, word: &str, source: Source) -> i32 {
        match scheme {
            SchemeType::Quanpin | SchemeType::Shuangpin => self.pinyin.cache_dynamic_candidate(scheme, code, word, source),
            // 五笔候选全部来自码表，没有云/AI 动态候选可缓存。
            SchemeType::Wubi => NO_MUTATION,
            SchemeType::JapaneseRomaji => {
                if code.is_empty() || word.is_empty() || source != Source::CloudSuggestion {
                    return MUTATION_FAILED;
                }
                let mut items = self.japanese_dynamic.get(code).unwrap_or_default();
                items.retain(|item| item.source != source);
                items.push(japanese::Candidate { pinyin: code.to_string(), canonical_pinyin: code.to_string(), word: word.to_string(), weight: 1, source });
                self.japanese_dynamic.insert(code, items);
                NO_MUTATION
            }
        }
    }

    pub fn cache_dynamic_candidate_for_request(&mut self, request: &QueryRequest, word: &str, source: Source) -> i32 {
        match request.scheme {
            SchemeType::Quanpin | SchemeType::Shuangpin => self.pinyin.cache_dynamic_candidate_for_request(request, word, source),
            SchemeType::Wubi => NO_MUTATION,
            SchemeType::JapaneseRomaji => self.cache_dynamic_candidate(request.scheme, &request.raw_input, word, source),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn registry() -> ProviderRegistry<'static> {
        let missing = Path::new("this-file-does-not-exist.db");
        let pinyin = PinyinProvider::open(crate::shuangpin::profile("xiaohe"), missing, "", Default::default(), None);
        ProviderRegistry::new(pinyin, missing, "", missing)
    }

    #[test]
    fn only_cloud_candidates_are_cached_for_japanese_and_one_per_source() {
        let mut registry = registry();
        let japanese = SchemeType::JapaneseRomaji;
        assert_eq!(registry.cache_dynamic_candidate(japanese, "nihon", "日本", Source::AiSuggestion), -1);
        assert_eq!(registry.cache_dynamic_candidate(japanese, "", "日本", Source::CloudSuggestion), -1);
        assert_eq!(registry.cache_dynamic_candidate(japanese, "nihon", "日本", Source::CloudSuggestion), 0);
        assert_eq!(registry.cache_dynamic_candidate(japanese, "nihon", "二本", Source::CloudSuggestion), 0);
        let cached = registry.japanese_dynamic.get("nihon").unwrap();
        assert_eq!(cached.len(), 1);
        assert_eq!(cached[0].word, "二本");
        registry.reset_cache(japanese);
        assert!(registry.japanese_dynamic.get("nihon").is_none());
    }

    #[test]
    fn wubi_and_japanese_have_no_writes_to_pretend() {
        let mut registry = registry();
        for scheme in [SchemeType::Wubi, SchemeType::JapaneseRomaji] {
            assert_eq!(registry.create_word(scheme, "a", "工"), 0);
        }
        assert_eq!(registry.cache_dynamic_candidate(SchemeType::Wubi, "a", "工", Source::CloudSuggestion), 0);
        assert_eq!(registry.update_weight_by_pinyin_and_word(SchemeType::JapaneseRomaji, "a", "工"), 0);
        // 五笔调频要写库；库不存在时失败，不假装成功。
        assert_eq!(registry.update_weight_by_pinyin_and_word(SchemeType::Wubi, "a", "工"), -1);
    }
}
