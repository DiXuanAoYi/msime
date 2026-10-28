cmake_minimum_required(VERSION 3.25)

project(MetasequoiaImeEngine VERSION 0.9.0 LANGUAGES CXX) # x-release-please-version

include(CTest)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

if(TARGET SQLite3::SQLite3)
    set(METASEQUOIA_IME_SQLITE_TARGET SQLite3::SQLite3)
else()
    set(METASEQUOIA_IME_SQLITE_TARGET SQLite::SQLite3)
endif()

get_target_property(METASEQUOIA_IME_SPDLOG_INCLUDE_DIRS spdlog::spdlog_header_only INTERFACE_INCLUDE_DIRECTORIES)

set(METASEQUOIA_IME_ENGINE_SOURCES
    common/helpcode_utils.cpp
    common/string_utils.cpp
    core/session.cpp
    capi/msime_capi.cpp
    core/nine_key_session.cpp
    core/candidate_queries.cpp
    core/punctuation_policy.cpp
    core/runtime_paths.cpp
    core/pinyin_decoder.cpp
    core/ime_session.cpp
    core/input_session.cpp
    core/input_session_composition.cpp
    core/input_session_editing.cpp
    core/input_session_candidates.cpp
    direct_helpcode/direct_lattice.cpp
    direct_helpcode/direct_resolver.cpp
    direct_helpcode/spelling_graph.cpp
    domain_dictionary/domain_dictionary.cpp
    english/english_dictionary.cpp
    japanese/japanese_matrix_search.cpp
    japanese/japanese_sentence_decoder.cpp
    japanese/romaji_converter.cpp
    local_modes/date_time_query.cpp
    local_modes/emoji_query.cpp
    local_modes/jianpin_query.cpp
    local_modes/kaomoji_query.cpp
    local_modes/local_database.cpp
    local_modes/quick_phrase_query.cpp
    local_modes/unicode_query.cpp
    local_modes/v_mode_query.cpp
    neural/sentence_model.cpp
    neural/neural_decoder.cpp
    neural/rescore_worker.cpp
    providers/japanese_candidate_provider.cpp
    providers/pinyin_candidate_provider.cpp
    providers/provider_registry.cpp
    providers/wubi_candidate_provider.cpp
    quanpin/engine.cpp
    quanpin/quanpin_dictionary.cpp
    quanpin/quanpin_query.cpp
    quanpin/quanpin_utils.cpp
    quanpin/lattice_rerank.cpp
    quanpin/word_lattice.cpp
    schemes/japanese_romaji_scheme.cpp
    schemes/quanpin_scheme.cpp
    schemes/shuangpin_scheme.cpp
    schemes/wubi_scheme.cpp
    shuangpin/engine.cpp
    shuangpin/shuangpin_custom_profile.cpp
    shuangpin/shuangpin_dictionary.cpp
    shuangpin/shuangpin_profile.cpp
    shuangpin/shuangpin_query.cpp
    shuangpin/shuangpin_utils.cpp
    user_dictionary/user_dictionary_journal.cpp
    user_dictionary/personal_dictionary.cpp
    user_dictionary/dictionary_state.cpp
    googlepinyinime-rev/src/share/dictbuilder.cpp
    googlepinyinime-rev/src/share/dictlist.cpp
    googlepinyinime-rev/src/share/dicttrie.cpp
    googlepinyinime-rev/src/share/lpicache.cpp
    googlepinyinime-rev/src/share/matrixsearch.cpp
    googlepinyinime-rev/src/share/mystdlib.cpp
    googlepinyinime-rev/src/share/ngram.cpp
    googlepinyinime-rev/src/share/pinyinime.cpp
    googlepinyinime-rev/src/share/searchutility.cpp
    googlepinyinime-rev/src/share/spellingtable.cpp
    googlepinyinime-rev/src/share/spellingtrie.cpp
    googlepinyinime-rev/src/share/splparser.cpp
    googlepinyinime-rev/src/share/sync.cpp
    googlepinyinime-rev/src/share/userdict.cpp
    googlepinyinime-rev/src/share/utf16char.cpp
    googlepinyinime-rev/src/share/utf16reader.cpp
)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" MSIME_NEURAL_PROCESSOR)
if(MSIME_NEURAL_PROCESSOR MATCHES "^(amd64|x86_64|x86|i[3-6]86)$")
    if(MSVC)
        list(APPEND METASEQUOIA_IME_ENGINE_SOURCES neural/int8_linear_avx2.cpp)
        set_property(SOURCE neural/int8_linear_avx2.cpp APPEND PROPERTY COMPILE_OPTIONS /arch:AVX2)
        set(MSIME_NEURAL_AVX2 1)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        list(APPEND METASEQUOIA_IME_ENGINE_SOURCES neural/int8_linear_avx2.cpp)
        set_property(SOURCE neural/int8_linear_avx2.cpp APPEND PROPERTY COMPILE_OPTIONS -mavx2 -mfma)
        set(MSIME_NEURAL_AVX2 1)
    endif()
endif()

# The vendored decoder accepts UTF-8 paths. Adapt its namespace-local file IO to native
# Windows paths without editing googlepinyinime-rev or changing the process locale.
if(WIN32)
    if(MSVC)
        set(METASEQUOIA_PINYIN_FILE_IO_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/core/pinyin_file_io.h")
    else()
        set(METASEQUOIA_PINYIN_FILE_IO_OPTIONS -include "${CMAKE_CURRENT_SOURCE_DIR}/core/pinyin_file_io.h")
    endif()
    set_property(SOURCE googlepinyinime-rev/src/share/dicttrie.cpp googlepinyinime-rev/src/share/userdict.cpp
        APPEND PROPERTY COMPILE_OPTIONS ${METASEQUOIA_PINYIN_FILE_IO_OPTIONS})
endif()

add_subdirectory(ngram)

add_library(MetasequoiaImeEngine STATIC ${METASEQUOIA_IME_ENGINE_SOURCES})
add_library(MetasequoiaIme::Engine ALIAS MetasequoiaImeEngine)

target_include_directories(MetasequoiaImeEngine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/utfcpp/source
    SYSTEM PRIVATE
        ${METASEQUOIA_IME_SPDLOG_INCLUDE_DIRS}
)

target_compile_definitions(MetasequoiaImeEngine PRIVATE FMT_HEADER_ONLY=1 SPDLOG_FMT_EXTERNAL)
if(MSIME_NEURAL_AVX2)
    target_compile_definitions(MetasequoiaImeEngine PRIVATE MSIME_NEURAL_AVX2=1)
endif()

target_link_libraries(MetasequoiaImeEngine
    PUBLIC
        Boost::headers
        fmt::fmt-header-only
        ${METASEQUOIA_IME_SQLITE_TARGET}
        Threads::Threads
        metasequoia_ime_ngram
)

if(WIN32)
    target_link_libraries(MetasequoiaImeEngine PUBLIC ole32 shell32)
endif()

if(MSVC)
    # NOMINMAX and the UNICODE pair stay PUBLIC: they change what the Windows headers declare and
    # what the TCHAR-flavoured entry points resolve to, so a consumer that disagrees with the engine
    # would be compiling against a different API surface. WIN32_LEAN_AND_MEAN is only an
    # include-time trim of Windows.h, and forcing it on consumers breaks any of them that go on to
    # include a header needing the fuller surface - WebView2.h, for one, relies on the `interface`
    # macro that combaseapi.h provides.
    target_compile_definitions(MetasequoiaImeEngine PUBLIC NOMINMAX UNICODE _UNICODE)
    target_compile_definitions(MetasequoiaImeEngine PRIVATE WIN32_LEAN_AND_MEAN)
    target_compile_options(MetasequoiaImeEngine PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(MetasequoiaImeEngine PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 黄金样本探针：把输入串清单喂给引擎并输出候选，作为其他实现（如 Rust 核心）的行为规格。只在显式指定目标时构建。
add_executable(msime_golden_probe EXCLUDE_FROM_ALL tools/golden_probe.cpp)
target_link_libraries(msime_golden_probe PRIVATE MetasequoiaIme::Engine)

# 调频探针：用合成词库跑真实的 adjust_candidate_ranking，输出调完的权重，当 Rust 调频算法的规格。
add_executable(msime_ranking_probe EXCLUDE_FROM_ALL tools/ranking_probe.cpp)
target_link_libraries(msime_ranking_probe PRIVATE MetasequoiaIme::Engine ${METASEQUOIA_IME_SQLITE_TARGET})

# 双拼与辅助码探针：导出方案表、解码、分段、取码与匹配的结果，当 Rust 实现的行为规格。
add_executable(msime_helpshuang_probe EXCLUDE_FROM_ALL tools/helpshuang_probe.cpp)
target_link_libraries(msime_helpshuang_probe PRIVATE MetasequoiaIme::Engine)

# 词格探针：对随机场景跑真实的 decode_word_lattice / merge_lattice_candidates，输出输入与结果，当 Rust 实现的规格。
add_executable(msime_lattice_probe EXCLUDE_FROM_ALL tools/lattice_probe.cpp)
target_link_libraries(msime_lattice_probe PRIVATE MetasequoiaIme::Engine)

# 语言模型探针：用真实的 kenlm 载入 sc.lm，导出词下标和逐词分数，当 Rust 读取器的规格。
add_executable(msime_lm_probe EXCLUDE_FROM_ALL tools/lm_probe.cpp)
target_link_libraries(msime_lm_probe PRIVATE MetasequoiaIme::Engine)

# 五笔与日语探针：导出罗马字转换、五笔方案与码表查询、日语词条/整句/候选提供者的结果，当 Rust 实现的行为规格。
add_executable(msime_wubi_japanese_probe EXCLUDE_FROM_ALL tools/wubi_japanese_probe.cpp)
target_link_libraries(msime_wubi_japanese_probe PRIVATE MetasequoiaIme::Engine ${METASEQUOIA_IME_SQLITE_TARGET})

# 英文词典与本地模式探针：导出 Unicode、V 模式、日期时间、表情、颜文字、超级简拼、快捷短语和英文词典的结果，当 Rust 实现的行为规格。
add_executable(msime_local_modes_probe EXCLUDE_FROM_ALL tools/local_modes_probe.cpp)
target_link_libraries(msime_local_modes_probe PRIVATE MetasequoiaIme::Engine ${METASEQUOIA_IME_SQLITE_TARGET})

# octagram .gram 探针：现建一份合成模型，用真实的 GramDb 跑编码与查询，当 Rust 读取器的行为规格。
add_executable(msime_gram_probe EXCLUDE_FROM_ALL tools/gram_probe.cpp)
target_link_libraries(msime_gram_probe PRIVATE MetasequoiaIme::Engine)

# 自定义双拼方案探针：对内置方案随机改动后用真实的 BuildCustomShuangpinProfile 构建，导出错误信息和方案表。
add_executable(msime_custom_shuangpin_probe EXCLUDE_FROM_ALL tools/custom_shuangpin_probe.cpp)
target_link_libraries(msime_custom_shuangpin_probe PRIVATE MetasequoiaIme::Engine)

# 双拼输入串处理探针：分段、转全拼、分隔符、全码辅助长度、句中辅助码解析，对九套内置方案跑随机输入串。
add_executable(msime_shuangpin_query_probe EXCLUDE_FROM_ALL tools/shuangpin_query_probe.cpp)
target_link_libraries(msime_shuangpin_query_probe PRIVATE MetasequoiaIme::Engine)

# 词格词库查询探针：真实的 make_lattice_db_lookup 在随包 msime.db 上查跨度（拼写归一、模糊变体、扣分、排序截断）。
add_executable(msime_lattice_lookup_probe EXCLUDE_FROM_ALL tools/lattice_lookup_probe.cpp)
target_link_libraries(msime_lattice_lookup_probe PRIVATE MetasequoiaIme::Engine)

# 直接辅助码拼写图探针：真实的 build_spelling_graph / single_path 跑随机输入串（辅码谓词用确定的哈希代替辅码表）。
add_executable(msime_spelling_graph_probe EXCLUDE_FROM_ALL tools/spelling_graph_probe.cpp)
target_link_libraries(msime_spelling_graph_probe PRIVATE MetasequoiaIme::Engine)

# 直接辅助码整句解码与解析器探针：真实的 decode_best_path / Resolver::resolve 在合成词库上跑随机输入串。
add_executable(msime_direct_helpcode_probe EXCLUDE_FROM_ALL tools/direct_helpcode_probe.cpp)
target_link_libraries(msime_direct_helpcode_probe PRIVATE MetasequoiaIme::Engine)

# 用户词库探针：真实的 user_dictionary::* / 个人词库 / 词库状态读取在合成词库上跑随机操作序列，导出返回值和库状态哈希。
add_executable(msime_user_dictionary_probe EXCLUDE_FROM_ALL tools/user_dictionary_probe.cpp)
target_link_libraries(msime_user_dictionary_probe PRIVATE MetasequoiaIme::Engine)

# 双拼词库探针：真实的 ShuangpinDictionary 在随包 msime.db 和合成小词库上跑查询、辅助码、造词/调频/删词，另带 std::sort 平局次序的样本。
add_executable(msime_shuangpin_dictionary_probe EXCLUDE_FROM_ALL tools/shuangpin_dictionary_probe.cpp)
target_link_libraries(msime_shuangpin_dictionary_probe PRIVATE MetasequoiaIme::Engine)

if(BUILD_TESTING)
    add_executable(test_segmentation_contract tests/src/test_segmentation_contract.cpp)
    target_link_libraries(test_segmentation_contract PRIVATE MetasequoiaIme::Engine)
    add_test(NAME segmentation_contract COMMAND test_segmentation_contract)

    add_executable(test_windows_ipc_contract contracts/tests/windows_ipc_contract.cpp)
    if(MSVC)
        target_compile_options(test_windows_ipc_contract PRIVATE /utf-8)
    endif()
    add_test(NAME windows_ipc_contract COMMAND test_windows_ipc_contract)

    add_executable(test_punctuation_contract contracts/tests/punctuation_contract.cpp)
    if(MSVC)
        target_compile_options(test_punctuation_contract PRIVATE /utf-8)
    endif()
    add_test(NAME punctuation_contract COMMAND test_punctuation_contract)

    add_executable(test_nine_key_session tests/src/test_nine_key_session.cpp)
    target_link_libraries(test_nine_key_session PRIVATE MetasequoiaImeEngine)
    add_test(NAME nine_key_session COMMAND test_nine_key_session)

    add_executable(test_platform_api tests/src/test_platform_api.cpp tests/src/test_public_session.cpp)
    target_link_libraries(test_platform_api PRIVATE MetasequoiaIme::Engine)
    add_test(NAME platform_api COMMAND test_platform_api)

    add_executable(test_data_path tests/src/test_data_path.cpp)
    target_link_libraries(test_data_path PRIVATE MetasequoiaIme::Engine)
    add_test(NAME data_path COMMAND test_data_path)

    add_executable(test_runtime_isolation tests/src/test_runtime_isolation.cpp)
    target_compile_definitions(test_runtime_isolation PRIVATE METASEQUOIA_TEST_PINYIN_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/googlepinyinime-rev/data/dict_pinyin.dat")
    target_link_libraries(test_runtime_isolation PRIVATE MetasequoiaIme::Engine)
    add_test(NAME runtime_isolation COMMAND test_runtime_isolation)

    add_executable(test_dictionary_state tests/src/test_dictionary_state.cpp)
    target_link_libraries(test_dictionary_state PRIVATE MetasequoiaImeEngine)
    add_test(NAME dictionary_state COMMAND test_dictionary_state)
    add_test(NAME dictionary_state_capacity COMMAND test_dictionary_state --capacity)

    add_executable(test_personal_dictionary tests/src/test_personal_dictionary.cpp)
    target_link_libraries(test_personal_dictionary PRIVATE MetasequoiaImeEngine)
    add_test(NAME personal_dictionary COMMAND test_personal_dictionary)

    add_executable(test_candidate_removal tests/src/test_candidate_removal.cpp)
    target_link_libraries(test_candidate_removal PRIVATE MetasequoiaIme::Engine)
    add_test(NAME candidate_removal COMMAND test_candidate_removal)

    # These two are the disk-bound outliers. Both spend their time building, staging and tearing
    # down dictionaries on disk rather than in the decoder, and Windows runs them 15-25x slower than
    # the other platforms: runtime_isolation takes 10.6s there against 0.70s on macOS, and
    # candidate_removal 7.6s against 0.30s. That leaves them alone in having single-digit headroom
    # under the Windows job's 60s default, and a runner that lost disk throughput has already pushed
    # runtime_isolation past it while the CPU-bound tests barely moved. A timeout is here to catch a
    # hang, which is unbounded, so it is set against a hang rather than against the runner's best
    # day; the other twenty tests keep the strict default.
    set_tests_properties(runtime_isolation candidate_removal PROPERTIES TIMEOUT 300)

    add_executable(test_fuzzy_pinyin tests/src/test_fuzzy_pinyin.cpp)
    target_link_libraries(test_fuzzy_pinyin PRIVATE MetasequoiaIme::Engine)
    add_test(NAME fuzzy_pinyin COMMAND test_fuzzy_pinyin)

    add_executable(test_domain_input tests/src/test_domain_input.cpp)
    target_link_libraries(test_domain_input PRIVATE MetasequoiaIme::Engine)
    add_test(NAME domain_input COMMAND test_domain_input)

    add_executable(test_octagram_scoring tests/src/test_octagram_scoring.cpp)
    target_link_libraries(test_octagram_scoring PRIVATE MetasequoiaIme::Engine)
    add_test(NAME octagram_scoring COMMAND test_octagram_scoring)

    add_executable(test_engine_smoke tests/src/test_engine_smoke.cpp)
    target_link_libraries(test_engine_smoke PRIVATE MetasequoiaIme::Engine)
    add_test(NAME engine_smoke COMMAND test_engine_smoke)

    # 神经整句排序集成测试：需数据目录里备好 sentence-model*.safetensors，缺文件时优雅跳过。
    add_executable(test_neural_sentence tests/src/test_neural_sentence.cpp)
    target_link_libraries(test_neural_sentence PRIVATE MetasequoiaIme::Engine)
    add_test(NAME neural_sentence COMMAND test_neural_sentence)

    if(MSIME_NEURAL_AVX2)
        add_executable(test_neural_int8_linear tests/src/test_neural_int8_linear.cpp)
        target_link_libraries(test_neural_int8_linear PRIVATE MetasequoiaIme::Engine)
        add_test(NAME neural_int8_linear COMMAND test_neural_int8_linear)
    endif()

    add_executable(test_input_session tests/src/test_input_session.cpp)
    target_compile_definitions(test_input_session PRIVATE METASEQUOIA_SKIP_FREQUENCY_TESTS)
    target_link_libraries(test_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME input_session COMMAND test_input_session)

    add_executable(test_frequency_input_session tests/src/test_input_session.cpp)
    target_compile_definitions(test_frequency_input_session PRIVATE METASEQUOIA_FREQUENCY_TESTS_ONLY)
    target_link_libraries(test_frequency_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME frequency_input_session COMMAND test_frequency_input_session)

    add_executable(test_direct_helpcode tests/src/test_direct_helpcode.cpp)
    target_link_libraries(test_direct_helpcode PRIVATE MetasequoiaIme::Engine)
    add_test(NAME direct_helpcode COMMAND test_direct_helpcode)

    add_executable(test_english_input_session tests/src/test_english_input_session.cpp)
    target_link_libraries(test_english_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME english_input_session COMMAND test_english_input_session)

    add_executable(test_quick_phrase_input_session tests/src/test_quick_phrase_input_session.cpp)
    target_link_libraries(test_quick_phrase_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME quick_phrase_input_session COMMAND test_quick_phrase_input_session)

    add_executable(test_mixed_expressive_input_session tests/src/test_mixed_expressive_input_session.cpp)
    target_link_libraries(test_mixed_expressive_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME mixed_expressive_input_session COMMAND test_mixed_expressive_input_session)

    add_executable(test_wubi_mixed_input_session tests/src/test_wubi_mixed_input_session.cpp)
    target_link_libraries(test_wubi_mixed_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME wubi_mixed_input_session COMMAND test_wubi_mixed_input_session)

    add_executable(test_jianpin_input_session tests/src/test_jianpin_input_session.cpp)
    target_link_libraries(test_jianpin_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME jianpin_input_session COMMAND test_jianpin_input_session)

    add_executable(test_temporary_input_session tests/src/test_temporary_input_session.cpp)
    target_link_libraries(test_temporary_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME temporary_input_session COMMAND test_temporary_input_session)

    add_executable(test_local_modes tests/src/test_local_modes.cpp)
    target_link_libraries(test_local_modes PRIVATE MetasequoiaIme::Engine)
    add_test(NAME local_modes COMMAND test_local_modes)

    # Per-keystroke latency against the real dictionaries; run by hand, not by ctest.
    add_executable(bench_input_session tests/src/bench_input_session.cpp)
    target_link_libraries(bench_input_session PRIVATE MetasequoiaIme::Engine)

    # Run this benchmark by hand with the released dictionaries and neural models.
    add_executable(bench_neural_rescore tests/src/bench_neural_rescore.cpp)
    target_link_libraries(bench_neural_rescore PRIVATE MetasequoiaIme::Engine)

    add_executable(test_online_input_session tests/src/test_online_input_session.cpp)
    target_link_libraries(test_online_input_session PRIVATE MetasequoiaIme::Engine)
    add_test(NAME online_input_session COMMAND test_online_input_session)
endif()

option(METASEQUOIA_IME_BUILD_VOICE "Build shared voice input capabilities" OFF)
if(METASEQUOIA_IME_BUILD_VOICE)
    add_subdirectory(voice)
endif()
