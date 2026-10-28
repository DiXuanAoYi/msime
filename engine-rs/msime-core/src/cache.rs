//! 定长缓存，对应 `engine/common/cache.h` 的 `CircularBuffer`：命中时 `find` 挪到最近使用端，
//! `get` 不挪；已有的键 `insert` 只换值、不动位置；满了丢最老的。容量只有一百多，线性查找就够。

use crate::lattice::{Item, Source};

#[derive(Debug, Clone)]
pub struct Cache<V> {
    capacity: usize,
    /// 最老的在前。
    entries: Vec<(String, V)>,
}

impl<V: Clone> Cache<V> {
    pub fn new(capacity: usize) -> Self {
        Cache { capacity, entries: Vec::new() }
    }

    pub fn insert(&mut self, key: &str, value: V) {
        if let Some(entry) = self.entries.iter_mut().find(|(existing, _)| existing == key) {
            entry.1 = value;
            return;
        }
        if self.entries.len() >= self.capacity {
            self.entries.remove(0);
        }
        self.entries.push((key.to_string(), value));
    }

    /// 命中时把条目挪到最近使用端。
    pub fn find(&mut self, key: &str) -> Option<V> {
        let at = self.entries.iter().position(|(existing, _)| existing == key)?;
        let entry = self.entries.remove(at);
        let value = entry.1.clone();
        self.entries.push(entry);
        Some(value)
    }

    /// 不动位置。
    pub fn get(&self, key: &str) -> Option<V> {
        self.entries.iter().find(|(existing, _)| existing == key).map(|(_, value)| value.clone())
    }

    pub fn contains(&self, key: &str) -> bool {
        self.entries.iter().any(|(existing, _)| existing == key)
    }

    pub fn clear(&mut self) {
        self.entries.clear();
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

/// 云/AI 候选插进缓存里的那份候选列表（`insert_word_to_series_cache` 一族）：同来源的云/AI 候选只留一条，
/// 词已经在就不再加；空列表直接放第一位，否则放第二位（AI 放第三位）。`reorder_ai` 为真时，云候选进来之后
/// 把已有的 AI 候选挪到第三位。
pub fn insert_dynamic_candidate(list: &mut Vec<Item>, pinyin: &str, word: &str, source: Source, reorder_ai: bool) {
    if source == Source::AiSuggestion || source == Source::CloudSuggestion {
        list.retain(|item| item.source != source);
    }
    if !list.iter().any(|item| item.word == word) {
        let new_item = crate::quanpin_dictionary::dynamic_item(pinyin, word, source);
        if list.is_empty() {
            list.push(new_item);
        } else {
            let index = if source == Source::AiSuggestion { 2.min(list.len()) } else { 1 };
            list.insert(index, new_item);
        }
    }
    if reorder_ai && source == Source::CloudSuggestion {
        if let Some(at) = list.iter().position(|item| item.source == Source::AiSuggestion) {
            let ai = list.remove(at);
            let index = 2.min(list.len());
            list.insert(index, ai);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_oldest_goes_first_and_find_refreshes() {
        let mut cache = Cache::new(2);
        cache.insert("a", 1);
        cache.insert("b", 2);
        assert_eq!(cache.find("a"), Some(1));
        cache.insert("c", 3);
        assert_eq!(cache.get("b"), None);
        assert_eq!(cache.get("a"), Some(1));
    }

    #[test]
    fn inserting_an_existing_key_keeps_its_place() {
        let mut cache = Cache::new(2);
        cache.insert("a", 1);
        cache.insert("b", 2);
        cache.insert("a", 9);
        cache.insert("c", 3);
        assert_eq!(cache.get("a"), None);
        assert_eq!(cache.get("b"), Some(2));
    }
}
