# 青简随包释义表

来源：https://github.com/qingjian-team/qingjian ，固定提交 4a3e963dfe299126574bfe3c370ff70a3f85ee77。
版权和许可：青简贡献者，GPL-3.0-or-later，完整许可见 LICENSE。
原始 TSV 保持不变；数据来源、机器生成误差与已知西语问题见 UPSTREAM-README.md。
水杉在 2026-10-11 增加 SQLite 查询适配：保留最多两条释义，以分号连接，日语读音单独标记为括号；个人修订优先。
源文件及摘要在 source.json；构建命令：python scripts/build_bundled_glossary.py --output <数据目录>/qingjian-glossary.db。
原始 TSV 与构建脚本随本仓源码发布；本地查询无需网络。英文候选的中文释义表为 glossary-zh.tsv。
