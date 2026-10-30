//! 全拼词库的分表命名。权威定义是 `engine/contracts/dictionary/format.json`：
//! 1–7 个音节用 `tbl_{N}_{首字母}`，8 个及以上用 `tbl_others_{首字母}`；
//! 首字母取第一个音节的第一个字母，只有 `abcdefghjklmnopqrstwxyz` 这些有表（没有 i、u、v 开头的音节）。

const INITIALS: &str = "abcdefghjklmnopqrstwxyz";
const MAXIMUM_NUMBERED_SYLLABLES: usize = 7;

pub fn table_name<S: AsRef<str>>(syllables: &[S]) -> Option<String> {
    let first = syllables.first()?.as_ref().chars().next()?;
    if !INITIALS.contains(first) {
        return None;
    }
    Some(if syllables.len() > MAXIMUM_NUMBERED_SYLLABLES {
        format!("tbl_others_{first}")
    } else {
        format!("tbl_{}_{first}", syllables.len())
    })
}

/// 词库里的键：音节之间用单引号连起来。
pub fn key<S: AsRef<str>>(syllables: &[S]) -> String {
    syllables.iter().map(|s| s.as_ref()).collect::<Vec<_>>().join("'")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numbers_tables_by_syllable_count_and_first_letter() {
        assert_eq!(table_name(&["ni"]).unwrap(), "tbl_1_n");
        assert_eq!(table_name(&["ni", "hao"]).unwrap(), "tbl_2_n");
        assert_eq!(table_name(&["shu", "ru", "fa"]).unwrap(), "tbl_3_s");
    }

    #[test]
    fn puts_eight_or_more_syllables_in_the_overflow_bucket() {
        let long = ["shui", "shan", "shu", "ru", "fa", "hai", "ke", "yi"];
        assert_eq!(table_name(&long).unwrap(), "tbl_others_s");
        assert_eq!(table_name(&long[..7]).unwrap(), "tbl_7_s");
    }

    #[test]
    fn has_no_table_for_letters_that_start_no_syllable() {
        assert_eq!(table_name(&["v"]), None);
        assert_eq!(table_name::<&str>(&[]), None);
    }

    #[test]
    fn joins_syllables_with_apostrophes() {
        assert_eq!(key(&["ni", "hao"]), "ni'hao");
    }
}
