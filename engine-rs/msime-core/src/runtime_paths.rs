//! 运行时目录与词库代际，移植自 `engine/core/runtime_paths.cpp` 和 `engine/user_dictionary/dictionary_state.cpp` 里的
//! `stage_dictionary_state`。
//!
//! 资源目录（只读，随包）、用户数据目录（持久）、缓存目录（可丢）三者不能重叠；可写的词库副本放在
//! `用户数据/dictionaries/<内容 ID>/`，先在 `<内容 ID>.incoming` 里备份、重放日志、写 `.ready`，再整体改名，
//! 失败不会动已有的代际。
//!
//! C++ 抛异常的地方这里返回 `Err`（内容只是原因，不和 C++ 的消息对齐）。

use crate::user_dictionary::{
    self, DictionaryStateRecord, PersonalDictionaryKind, ReplayResult, RuntimePaths, ENGLISH_DICTIONARY, MAIN_DICTIONARY, USER_JOURNAL,
};
use rusqlite::{backup::Backup, params, Connection, OpenFlags};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

/// 四个根目录：资源、用户数据、缓存、当前代际的词库目录。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuntimeRoots {
    pub resources: PathBuf,
    pub user_data: PathBuf,
    pub cache: PathBuf,
    pub dictionaries: PathBuf,
}

impl RuntimeRoots {
    /// 用户词库那一层用的两个目录。
    pub fn user_paths(&self) -> RuntimePaths {
        RuntimePaths { user_data: self.user_data.clone(), dictionaries: self.dictionaries.clone() }
    }

    pub fn validate(&self) -> Result<(), String> {
        if [&self.resources, &self.user_data, &self.cache, &self.dictionaries].iter().all(|path| path.is_absolute()) {
            Ok(())
        } else {
            Err("Runtime directories must be absolute".into())
        }
    }
}

/// 目录不存在时也能给出规范路径：最深的已存在祖先取真实路径，余下的部分接在后面（对应 `std::filesystem::weakly_canonical`）。
fn weakly_canonical(path: &Path) -> PathBuf {
    let mut rest: Vec<std::ffi::OsString> = Vec::new();
    let mut base = path.to_path_buf();
    loop {
        if let Ok(real) = fs::canonicalize(&base) {
            let mut result = real;
            for part in rest.iter().rev() {
                result.push(part);
            }
            return normalize(&result);
        }
        match (base.file_name().map(|n| n.to_os_string()), base.parent().map(Path::to_path_buf)) {
            (Some(name), Some(parent)) => {
                rest.push(name);
                base = parent;
            }
            _ => return normalize(path),
        }
    }
}

fn normalize(path: &Path) -> PathBuf {
    use std::path::Component;
    let mut result = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                result.pop();
            }
            other => result.push(other.as_os_str()),
        }
    }
    result
}

/// 一个目录是不是另一个的前缀（含相等）。
fn roots_overlap(first: &Path, second: &Path) -> bool {
    first.starts_with(second) || second.starts_with(first)
}

fn copy_database(source: &Path, target: &Path) -> Result<(), String> {
    // SQLite 的备份接口会带上已提交的 WAL 内容；只拷 .db 会丢。
    let failed = |_| format!("Unable to copy runtime dictionary: {}", source.display());
    let input = Connection::open_with_flags(source, OpenFlags::SQLITE_OPEN_READ_ONLY).map_err(failed)?;
    let mut output = Connection::open_with_flags(target, OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE).map_err(failed)?;
    let backup = Backup::new(&input, &mut output).map_err(failed)?;
    backup.run_to_completion(i32::MAX, Duration::ZERO, None).map_err(failed)
}

fn replayed(result: ReplayResult) -> Result<(), String> {
    if result.failed != 0 || !result.error.is_empty() {
        return Err(format!("Runtime dictionary replay failed: {}", result.error));
    }
    Ok(())
}

fn valid_content_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 128 && id.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_')
}

/// 准备（或切回）一代词库：已就绪的代际重放一遍日志（别的代际学到的东西要跟过来），否则在 `.incoming` 里备份、重放、
/// 写就绪标记、整体改名。三个根目录必须互不重叠。
pub fn prepare_runtime_paths(resources: &Path, user_data: &Path, cache: &Path, content_id: &str) -> Result<RuntimeRoots, String> {
    if !valid_content_id(content_id) {
        return Err("Invalid runtime content ID".into());
    }
    let result = RuntimeRoots {
        resources: resources.to_path_buf(),
        user_data: user_data.to_path_buf(),
        cache: cache.to_path_buf(),
        dictionaries: user_data.join("dictionaries").join(content_id),
    };
    result.validate()?;
    // 各个根目录的生命周期不同：先校验再创建，清缓存不能删到持久数据，准备也不能写进不可变的资源。
    let (resource_root, user_root, cache_root) = (weakly_canonical(resources), weakly_canonical(user_data), weakly_canonical(cache));
    if roots_overlap(&resource_root, &user_root) || roots_overlap(&resource_root, &cache_root) || roots_overlap(&user_root, &cache_root) {
        return Err("Resource, user-data and cache directories must not overlap".into());
    }
    fs::create_dir_all(user_data).map_err(|e| e.to_string())?;
    fs::create_dir_all(cache).map_err(|e| e.to_string())?;
    let journal = user_data.join(USER_JOURNAL).to_string_lossy().into_owned();
    let marker = result.dictionaries.join(".ready");
    if marker.exists() {
        for name in [MAIN_DICTIONARY, ENGLISH_DICTIONARY] {
            if !result.dictionaries.join(name).is_file() {
                return Err("Incomplete runtime dictionary generation".into());
            }
        }
        // 宿主可能切回之前准备好的代际：把当前日志再重放一遍，新代际上学到的改动才不会丢。
        replayed(user_dictionary::replay(
            &journal,
            &result.dictionaries.join(MAIN_DICTIONARY).to_string_lossy(),
            &result.dictionaries.join(ENGLISH_DICTIONARY).to_string_lossy(),
        ))?;
        return Ok(result);
    }
    let stage = result.dictionaries.parent().expect("词库目录有上一级").join(format!("{content_id}.incoming"));
    fs::create_dir_all(stage.parent().expect("暂存目录有上一级")).map_err(|e| e.to_string())?;
    // 独占地建目录，同一个 ID 的准备不会重叠。
    fs::create_dir(&stage).map_err(|_| "Runtime dictionary staging directory already exists".to_string())?;
    let built = (|| -> Result<(), String> {
        for name in [MAIN_DICTIONARY, ENGLISH_DICTIONARY] {
            copy_database(&resources.join(name), &stage.join(name))?;
        }
        replayed(user_dictionary::replay(&journal, &stage.join(MAIN_DICTIONARY).to_string_lossy(), &stage.join(ENGLISH_DICTIONARY).to_string_lossy()))?;
        fs::write(stage.join(".ready"), format!("{content_id}\n")).map_err(|_| "Cannot finalize runtime dictionary generation".to_string())?;
        fs::rename(&stage, &result.dictionaries).map_err(|e| e.to_string())
    })();
    if let Err(error) = built {
        let _ = fs::remove_dir_all(&stage);
        return Err(error);
    }
    Ok(result)
}

fn bounded_text(value: &str, maximum: usize, allow_empty: bool) -> Result<(), String> {
    if (allow_empty || !value.is_empty()) && value.len() <= maximum && !value.contains('\0') {
        Ok(())
    } else {
        Err("Invalid or unavailable dictionary state".into())
    }
}

fn kind_label(kind: PersonalDictionaryKind) -> &'static str {
    match kind {
        PersonalDictionaryKind::Pinyin => "pinyin",
        PersonalDictionaryKind::Wubi => "wubi",
        PersonalDictionaryKind::English => "english",
        PersonalDictionaryKind::QuickPhrase => "quick",
    }
}

/// 在新建的代际目录里一次备好整套替换：重建日志（词条、固定位置、选择计数）和词库。`next` 在校验过的结尾返回
/// `Ok(None)`，遇到坏数据或取消返回 `Err`。身份或位置重复、数值越界、超过 `maximum_records` 都整体失败，
/// 失败时删掉本次新建的目录，已有代际不动。准备好不等于切过去，也不删旧代际。
pub fn stage_dictionary_state(
    resources: &Path,
    generation: &Path,
    content_id: &str,
    next: &mut dyn FnMut() -> Result<Option<DictionaryStateRecord>, String>,
    maximum_records: usize,
) -> Result<RuntimeRoots, String> {
    if !resources.is_absolute() || !generation.is_absolute() {
        return Err("Invalid or unavailable dictionary state".into());
    }
    if roots_overlap(&weakly_canonical(resources), &weakly_canonical(generation)) {
        return Err("Invalid or unavailable dictionary state".into());
    }
    fs::create_dir(generation).map_err(|_| "Invalid or unavailable dictionary state".to_string())?;
    let built = (|| -> Result<RuntimeRoots, String> {
        let user = generation.join("user");
        fs::create_dir(&user).map_err(|e| e.to_string())?;
        let journal = user.join(USER_JOURNAL);
        if !user_dictionary::ensure_user_database(&journal.to_string_lossy()) {
            return Err("Invalid or unavailable dictionary state".into());
        }
        {
            let invalid = |_| "Invalid or unavailable dictionary state".to_string();
            let db = Connection::open_with_flags(&journal, OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_FULL_MUTEX).map_err(invalid)?;
            db.busy_timeout(Duration::from_millis(5000)).map_err(invalid)?;
            db.execute_batch("BEGIN IMMEDIATE").map_err(invalid)?;
            {
                let mut entry = db
                    .prepare("INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight,display,user_inserted) VALUES(?1,?2,?3,?4,?5,?6,?7)")
                    .map_err(invalid)?;
                let mut position = db.prepare("INSERT INTO fixed_candidate_positions(context_key,entry_key,value,position) VALUES(?1,?2,?3,?4)").map_err(invalid)?;
                let mut selection = db.prepare("INSERT INTO candidate_selection_state(context_key,entry_key,value,selection_count) VALUES(?1,?2,?3,?4)").map_err(invalid)?;
                let mut count = 0usize;
                while let Some(record) = next()? {
                    count += 1;
                    if count > maximum_records {
                        return Err("Invalid or unavailable dictionary state".into());
                    }
                    match record {
                        DictionaryStateRecord::Entry(value) => {
                            bounded_text(&value.key, 512, false)?;
                            bounded_text(&value.value, 4096, false)?;
                            bounded_text(&value.display, 4096, true)?;
                            if !(0..=100_000_000).contains(&value.weight) {
                                return Err("Invalid or unavailable dictionary state".into());
                            }
                            entry
                                .execute(params![
                                    kind_label(value.kind),
                                    value.key,
                                    value.value,
                                    if value.deleted { "delete" } else { "upsert" },
                                    value.weight,
                                    value.display,
                                    i32::from(value.user_inserted)
                                ])
                                .map_err(invalid)?;
                        }
                        DictionaryStateRecord::Position(value) => {
                            bounded_text(&value.key, 512, false)?;
                            bounded_text(&value.value, 4096, false)?;
                            bounded_text(&value.context, 512, false)?;
                            if !(1..=5).contains(&value.position) {
                                return Err("Invalid or unavailable dictionary state".into());
                            }
                            position.execute(params![value.context, value.key, value.value, value.position]).map_err(invalid)?;
                        }
                        DictionaryStateRecord::Selection(value) => {
                            bounded_text(&value.key, 512, false)?;
                            bounded_text(&value.value, 4096, false)?;
                            bounded_text(&value.context, 512, false)?;
                            if !(0..=10).contains(&value.count) {
                                return Err("Invalid or unavailable dictionary state".into());
                            }
                            selection.execute(params![value.context, value.key, value.value, value.count]).map_err(invalid)?;
                        }
                    }
                }
            }
            db.execute_batch("COMMIT").map_err(invalid)?;
        }
        prepare_runtime_paths(resources, &user, &generation.join("cache"), content_id)
    })();
    if built.is_err() {
        let _ = fs::remove_dir_all(generation);
    }
    built
}
