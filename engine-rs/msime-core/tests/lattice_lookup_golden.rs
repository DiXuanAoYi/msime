//! 词格词库查询对着真实的 C++ `quanpin::make_lattice_db_lookup` 核对。基线 `expected_lattice_lookup.jsonl` 由
//! `msime_lattice_lookup_probe` 在随包的 `msime.db` 上导出：700 个随机跨度加全部单音节键共 1119 个（随机跨度取自词库真有的键，混入 ü 的另一种写法、
//! 没有的键和不完整的音节），每个配随机的模糊音规则和跨度上限。需要词库：`installer/app_data/msime.db`（或 `MSIME_DB`），
//! 缺了就跳过并打印原因。

use msime_core::Dictionary;
use serde_json::Value;
use std::path::PathBuf;

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

struct Fnv(u64);

impl Fnv {
    fn field(&mut self, text: &str) {
        for b in text.bytes().chain(std::iter::once(0xFF)) {
            self.0 ^= u64::from(b);
            self.0 = self.0.wrapping_mul(0x100000001b3);
        }
    }
}

#[test]
fn lattice_lookup_equals_cpp() {
    let db = std::env::var_os("MSIME_DB").map(PathBuf::from).unwrap_or_else(|| root().join("installer/app_data/msime.db"));
    if !db.exists() {
        eprintln!("跳过：没有 {}", db.display());
        return;
    }
    let dictionary = Dictionary::open(&db).unwrap();
    let text = std::fs::read_to_string(root().join("engine/contracts/golden/expected_lattice_lookup.jsonl")).unwrap();
    let (mut checked, mut nonempty, mut penalized, mut truncated, mut multi_variant) = (0, 0, 0, 0, 0);
    for row in text.lines().filter(|l| !l.trim().is_empty()).map(|l| serde_json::from_str::<Value>(l).unwrap()) {
        let span: Vec<String> = row["span"].as_array().unwrap().iter().map(|s| s.as_str().unwrap().to_string()).collect();
        let (limit, rules) = (row["limit"].as_i64().unwrap() as i32, row["rules"].as_u64().unwrap() as u32);
        let lexemes = dictionary.lattice_lookup(&span, limit, rules).unwrap();

        let mut hash = Fnv(0xcbf29ce484222325);
        for l in &lexemes {
            hash.field(&l.key);
            hash.field(&l.value);
            hash.field(&l.weight.to_string());
            hash.field(&format!("{}", l.penalty));
        }
        assert_eq!(lexemes.len() as u64, row["n"].as_u64().unwrap(), "条数 {row}");
        assert_eq!(format!("{:016x}", hash.0), row["h"].as_str().unwrap(), "内容哈希 {row}");
        for (got, want) in lexemes.iter().zip(row["top"].as_array().unwrap()) {
            assert_eq!(
                (got.key.as_str(), got.value.as_str(), got.weight, got.penalty),
                (want[0].as_str().unwrap(), want[1].as_str().unwrap(), want[2].as_i64().unwrap(), want[3].as_f64().unwrap()),
                "{row}"
            );
        }
        checked += 1;
        nonempty += i32::from(!lexemes.is_empty());
        penalized += i32::from(lexemes.iter().any(|l| l.penalty > 0.0));
        truncated += i32::from(lexemes.len() as i32 >= limit);
        multi_variant += i32::from(rules != 0);
    }
    assert!(checked >= 1100 && nonempty > 1000 && penalized > 150 && truncated > 300 && multi_variant > 800, "{checked}/{nonempty}/{penalized}/{truncated}/{multi_variant}");
    eprintln!("词格词库查询：{checked} 个跨度（非空 {nonempty}，带模糊扣分 {penalized}，吃满上限 {truncated}，开模糊音 {multi_variant}）的条数、内容哈希、前 12 项与 C++ 一致");
}
