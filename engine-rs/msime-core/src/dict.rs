//! 全拼词库（`msime.db`）的只读查询。

use crate::table::{key, table_name};
use rusqlite::{Connection, OpenFlags};
use std::path::Path;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    pub text: String,
    pub weight: i64,
}

pub struct Dictionary {
    connection: Connection,
}

impl Dictionary {
    /// 只读打开，不会改动词库，也不会产生 -wal 文件。
    pub fn open(path: &Path) -> rusqlite::Result<Self> {
        let connection = Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_URI)?;
        Ok(Self { connection })
    }

    /// 这一串音节对应的词，按权重从高到低；权重相同按词库里的先后（rowid）。
    /// 表不存在（没有这个首字母的表）返回空。
    pub fn exact<S: AsRef<str>>(&self, syllables: &[S]) -> rusqlite::Result<Vec<Entry>> {
        let Some(table) = table_name(syllables) else { return Ok(Vec::new()) };
        let sql = format!("select value, weight from {table} where key = ?1 order by weight desc, rowid");
        let mut statement = match self.connection.prepare(&sql) {
            Ok(statement) => statement,
            Err(rusqlite::Error::SqliteFailure(_, Some(message))) if message.starts_with("no such table") => {
                return Ok(Vec::new())
            }
            Err(other) => return Err(other),
        };
        let rows = statement.query_map([key(syllables)], |row| Ok(Entry { text: row.get(0)?, weight: row.get(1)? }))?;
        rows.collect()
    }
}
