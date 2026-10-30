//! 输入会话：按键 → 方案 → 请求 → 提供者 → 候选列表，对应 `engine/core/ime_session.cpp`。
//!
//! 会话持有方案和提供者登记处，把会话级开关写进每次请求，查询后维护当前的预编辑、请求和候选；
//! 五笔拼音混输时把同一串字母同时交给码表和全拼，合并两边的候选。
//!
//! 双拼直接辅助码开着时，请求在查询前由词典层的解析器改写成句中辅助码的形状。

use crate::domain_dictionary::normalize_enabled;
use crate::lattice::{Item, Source};
use crate::provider::ProviderRegistry;
use crate::quanpin_dictionary::SentenceAssociation;
use crate::request::{QueryRequest, SchemeType};
use crate::scheme::{InputScheme, JapaneseRomajiScheme, QuanpinScheme, ShuangpinScheme, WubiScheme};
use crate::shuangpin::Profile;
use crate::shuangpin_query as sq;
use std::collections::HashSet;

/// 五笔输入选项：拼音混输、z 通配。
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct WubiInputOptions {
    pub mixed_pinyin: bool,
    pub z_wildcard: bool,
}

#[derive(Default)]
struct CompositionState {
    preedit: String,
    request: QueryRequest,
    candidates: Vec<Item>,
}

/// 当前方案。用枚举而不是 trait 对象，五笔和双拼的专有开关才够得着。
enum Scheme {
    Quanpin(QuanpinScheme),
    Shuangpin(ShuangpinScheme),
    Wubi(WubiScheme),
    Japanese(JapaneseRomajiScheme),
}

impl Scheme {
    fn inner(&self) -> &dyn InputScheme {
        match self {
            Scheme::Quanpin(s) => s,
            Scheme::Shuangpin(s) => s,
            Scheme::Wubi(s) => s,
            Scheme::Japanese(s) => s,
        }
    }

    fn inner_mut(&mut self) -> &mut dyn InputScheme {
        match self {
            Scheme::Quanpin(s) => s,
            Scheme::Shuangpin(s) => s,
            Scheme::Wubi(s) => s,
            Scheme::Japanese(s) => s,
        }
    }
}

/// 双码辅助开着时请求里的分段要带上辅助码：`xxxx'AB`。探测在去掉分隔符的空间里做，所以切分也在那里做。
fn apply_shuangpin_helpcode_segmentation(request: &mut QueryRequest, profile: &Profile) {
    if request.scheme != SchemeType::Shuangpin
        || !request.enable_shuangpin_helpcode
        || sq::detect_active_double_helpcode_length(&request.raw_input, &request.raw_input_with_cases, profile) != 2
    {
        return;
    }
    let helpcode_length = 2;
    let base_raw_input = sq::trim_trailing_letters_preserve_delimiters(&request.raw_input, helpcode_length);
    let base_raw_input_with_cases = sq::trim_trailing_letters_preserve_delimiters(&request.raw_input_with_cases, helpcode_length);
    let base_segmentation = sq::segment_input(&base_raw_input, profile);
    let effective_input_with_cases = sq::remove_manual_delimiters(&request.raw_input_with_cases);
    let help_codes = &effective_input_with_cases[effective_input_with_cases.len() - helpcode_length..];
    request.raw_segmentation = format!("{}'{help_codes}", sq::apply_segmentation_cases(&base_segmentation, &base_raw_input_with_cases));
    request.normalized_segmentation = format!("{}'{help_codes}", sq::to_quanpin_segmentation(&base_segmentation, profile));
    request.segmentation = request.normalized_segmentation.clone();
}

pub struct ImeSession<'a> {
    registry: ProviderRegistry<'a>,
    profile: &'static Profile,
    scheme: Scheme,
    state: CompositionState,
    enable_shuangpin_helpcode: bool,
    enable_mid_sentence_helpcode: bool,
    enable_direct_helpcode: bool,
    direct_helpcode_slash_marker: bool,
    direct_helpcode_uppercase_marker: bool,
    enable_mid_sentence_uppercase_trigger: bool,
    enable_quanpin_helpcode: bool,
    quanpin_autocorrect_types: u32,
    fuzzy_pinyin: u32,
    sentence_association: SentenceAssociation,
    enabled_domain_dictionaries: String,
    rescoring_context: String,
    wubi_options: WubiInputOptions,
}

impl<'a> ImeSession<'a> {
    pub fn new(scheme_type: SchemeType, profile: &'static Profile, registry: ProviderRegistry<'a>) -> Self {
        let mut session = ImeSession {
            registry,
            profile,
            scheme: Scheme::Quanpin(QuanpinScheme::default()),
            state: CompositionState::default(),
            enable_shuangpin_helpcode: false,
            enable_mid_sentence_helpcode: false,
            enable_direct_helpcode: false,
            direct_helpcode_slash_marker: true,
            direct_helpcode_uppercase_marker: false,
            enable_mid_sentence_uppercase_trigger: false,
            enable_quanpin_helpcode: false,
            quanpin_autocorrect_types: 0,
            fuzzy_pinyin: 0,
            sentence_association: SentenceAssociation::default(),
            enabled_domain_dictionaries: String::new(),
            rescoring_context: String::new(),
            wubi_options: WubiInputOptions::default(),
        };
        session.scheme = session.create_scheme(scheme_type);
        session.bind_wubi_scheme();
        session.bind_shuangpin_scheme();
        session.bind_quanpin_scheme();
        session
    }

    fn create_scheme(&self, scheme_type: SchemeType) -> Scheme {
        match scheme_type {
            SchemeType::Shuangpin => Scheme::Shuangpin(ShuangpinScheme::new(self.profile)),
            SchemeType::Quanpin => Scheme::Quanpin(QuanpinScheme::default()),
            SchemeType::Wubi => Scheme::Wubi(WubiScheme::default()),
            SchemeType::JapaneseRomaji => Scheme::Japanese(JapaneseRomajiScheme::default()),
        }
    }

    /// 句中辅助码的大写触发：开了句中辅助码、勾了大写、没开直接辅助码。
    pub fn mid_sentence_uppercase_trigger_active(&self) -> bool {
        self.enable_mid_sentence_helpcode && self.enable_mid_sentence_uppercase_trigger && !self.enable_direct_helpcode
    }

    fn configure_shuangpin_scheme(&self, scheme: &mut Scheme) {
        if let Scheme::Shuangpin(shuangpin) = scheme {
            shuangpin.set_direct_helpcode(self.enable_direct_helpcode);
            shuangpin.set_mid_sentence_uppercase_trigger(self.mid_sentence_uppercase_trigger_active());
        }
    }

    fn bind_shuangpin_scheme(&mut self) {
        let mut scheme = std::mem::replace(&mut self.scheme, Scheme::Quanpin(QuanpinScheme::default()));
        self.configure_shuangpin_scheme(&mut scheme);
        self.scheme = scheme;
    }

    /// 把会话的纠错掩码下发给全拼方案（别的方案什么也不做）。要在 build_request 之前调，掩码决定切分要不要跑手误别名层。
    fn configure_quanpin_scheme(&self, scheme: &mut Scheme) {
        if let Scheme::Quanpin(quanpin) = scheme {
            quanpin.set_autocorrect_types(self.quanpin_autocorrect_types);
        }
    }

    /// 换方案或改纠错开关时把掩码绑到当前方案上。
    fn bind_quanpin_scheme(&mut self) {
        let mut scheme = std::mem::replace(&mut self.scheme, Scheme::Quanpin(QuanpinScheme::default()));
        self.configure_quanpin_scheme(&mut scheme);
        self.scheme = scheme;
    }

    fn bind_wubi_scheme(&mut self) {
        let options = self.wubi_options;
        if let Scheme::Wubi(wubi) = &mut self.scheme {
            wubi.set_mixed_pinyin_allowed(options.mixed_pinyin);
            wubi.set_z_wildcard(options.z_wildcard);
        }
    }

    // ------------------------------------------------------------------------------------------
    // 会话级开关
    // ------------------------------------------------------------------------------------------

    pub fn set_direct_helpcode_enabled(&mut self, enabled: bool) {
        self.enable_direct_helpcode = enabled;
        self.bind_shuangpin_scheme();
    }

    pub fn direct_helpcode_enabled(&self) -> bool {
        self.enable_direct_helpcode
    }

    pub fn set_direct_helpcode_markers(&mut self, slash: bool, uppercase: bool) {
        self.direct_helpcode_slash_marker = slash;
        self.direct_helpcode_uppercase_marker = uppercase;
    }

    pub fn set_mid_sentence_helpcode_enabled(&mut self, enabled: bool) {
        self.enable_mid_sentence_helpcode = enabled;
        self.bind_shuangpin_scheme();
    }

    pub fn set_mid_sentence_uppercase_trigger_enabled(&mut self, enabled: bool) {
        self.enable_mid_sentence_uppercase_trigger = enabled;
        self.bind_shuangpin_scheme();
    }

    pub fn set_shuangpin_helpcode_enabled(&mut self, enabled: bool) {
        self.enable_shuangpin_helpcode = enabled;
    }

    pub fn set_quanpin_helpcode_enabled(&mut self, enabled: bool) {
        self.enable_quanpin_helpcode = enabled;
    }

    pub fn set_quanpin_autocorrect_types(&mut self, types: u32) {
        self.quanpin_autocorrect_types = types;
        self.bind_quanpin_scheme();
    }

    pub fn set_fuzzy_pinyin_options(&mut self, rules: u32) {
        self.fuzzy_pinyin = rules;
    }

    pub fn set_sentence_association(&mut self, options: SentenceAssociation) {
        self.sentence_association = options;
    }

    pub fn set_rescoring_context(&mut self, context: &str) {
        self.rescoring_context = context.to_string();
    }

    pub fn set_enabled_domain_dictionaries(&mut self, csv: &str) {
        let normalized = normalize_enabled(csv);
        if self.enabled_domain_dictionaries == normalized {
            return;
        }
        self.enabled_domain_dictionaries = normalized;
        self.reset_cache();
    }

    pub fn set_wubi_input_options(&mut self, options: WubiInputOptions) {
        self.wubi_options = options;
        self.bind_wubi_scheme();
    }

    pub fn wubi_input_options(&self) -> WubiInputOptions {
        self.wubi_options
    }

    pub fn switch_scheme(&mut self, scheme_type: SchemeType) {
        self.scheme = self.create_scheme(scheme_type);
        self.bind_wubi_scheme();
        self.bind_shuangpin_scheme();
        self.bind_quanpin_scheme();
        self.state = CompositionState::default();
    }

    pub fn current_scheme_type(&self) -> SchemeType {
        self.scheme.inner().scheme_type()
    }

    pub fn registry_mut(&mut self) -> &mut ProviderRegistry<'a> {
        &mut self.registry
    }

    // ------------------------------------------------------------------------------------------
    // 按键与整串替换
    // ------------------------------------------------------------------------------------------

    pub fn handle_key(&mut self, vk: u32, modifiers_down: u32, wch: u16) {
        self.scheme.inner_mut().handle_key(vk, modifiers_down, wch);
        self.refresh_candidates();
    }

    /// 宿主整串替换当前方案的原始输入（中间编辑、光标前缀解码）。五笔的长度上限取设置而不是上一次查询的结果。
    pub fn replace_active_raw_input(&mut self, raw_input: &str, raw_input_with_cases: &str) {
        let mixed = self.wubi_options.mixed_pinyin;
        if let Scheme::Wubi(wubi) = &mut self.scheme {
            wubi.set_extended_length_allowed(mixed);
        }
        self.scheme.inner_mut().set_raw_input(raw_input, raw_input_with_cases);
        self.refresh_candidates();
    }

    pub fn reset(&mut self) {
        self.scheme.inner_mut().reset();
        self.state = CompositionState::default();
    }

    pub fn reset_cache(&mut self) {
        self.registry.reset_cache(self.current_scheme_type());
        // 混输组合同时查过拼音提供者，清缓存必须两边都清，否则选中调频后的重排会读到旧序。
        if matches!(self.scheme, Scheme::Wubi(_)) && self.wubi_options.mixed_pinyin {
            self.registry.reset_cache(SchemeType::Quanpin);
        }
        self.refresh_candidates();
    }

    pub fn reset_sentence_cache(&self) {
        self.registry.reset_sentence_cache();
    }

    // ------------------------------------------------------------------------------------------
    // 词库写入与动态候选
    // ------------------------------------------------------------------------------------------

    pub fn create_word(&mut self, pinyin: &str, word: &str) -> i32 {
        let scheme = self.current_scheme_type();
        self.registry.create_word(scheme, pinyin, word)
    }

    pub fn update_weight_by_pinyin_and_word(&mut self, scheme: SchemeType, pinyin: &str, word: &str) -> i32 {
        self.registry.update_weight_by_pinyin_and_word(scheme, pinyin, word)
    }

    pub fn delete_by_pinyin_and_word(&mut self, scheme: SchemeType, pinyin: &str, word: &str) -> i32 {
        self.registry.delete_by_pinyin_and_word(scheme, pinyin, word)
    }

    pub fn cache_dynamic_candidate(&mut self, pinyin: &str, word: &str, source: Source) -> i32 {
        let scheme = self.current_scheme_type();
        self.registry.cache_dynamic_candidate(scheme, pinyin, word, source)
    }

    pub fn cache_dynamic_candidate_for_current_request(&mut self, word: &str, source: Source) -> i32 {
        let request = self.state.request.clone();
        self.registry.cache_dynamic_candidate_for_request(&request, word, source)
    }

    /// 缓存成功就刷新候选，动态候选马上出现在列表里。
    pub fn apply_dynamic_candidate(&mut self, word: &str, source: Source) -> i32 {
        let result = self.cache_dynamic_candidate_for_current_request(word, source);
        if result == 0 {
            self.refresh_candidates();
        }
        result
    }

    pub fn find_candidate(&self, scheme: SchemeType, key: &str, value: &str) -> Option<Item> {
        self.registry.find_candidate(scheme, key, value)
    }

    /// 容错仲裁选中读音后的分段显示（大小写原样）；空表示当前串没有容错布局。
    pub fn fault_segmentation_with_cases(&self) -> &str {
        self.registry.fault_segmentation_with_cases()
    }

    pub fn expand_initial_candidates(&mut self) -> bool {
        let request = self.state.request.clone();
        self.registry.expand_initial_candidates(&request, &mut self.state.candidates)
    }

    // ------------------------------------------------------------------------------------------
    // 读出状态
    // ------------------------------------------------------------------------------------------

    pub fn preedit(&self) -> &str {
        &self.state.preedit
    }

    pub fn request(&self) -> &QueryRequest {
        &self.state.request
    }

    pub fn candidates(&self) -> &[Item] {
        &self.state.candidates
    }

    pub fn wubi_code_is_complete(&self) -> bool {
        matches!(&self.scheme, Scheme::Wubi(wubi) if wubi.has_complete_code())
    }

    /// 用一个临时方案查一串原始输入，不动当前的组合（光标前缀解码）。
    pub fn query_raw_candidates(&mut self, raw_input: &str, raw_input_with_cases: &str) -> Vec<Item> {
        // 临时方案要按同样的触发规则解析原串（大写段），否则前缀查询与正式查询切得不一样。
        let mut query_scheme = self.create_scheme(self.current_scheme_type());
        self.configure_shuangpin_scheme(&mut query_scheme);
        self.configure_quanpin_scheme(&mut query_scheme);
        query_scheme.inner_mut().set_raw_input(raw_input, raw_input_with_cases);
        let mut request = query_scheme.inner().build_request();
        self.apply_request_options(&mut request);
        apply_shuangpin_helpcode_segmentation(&mut request, self.profile);
        self.resolve_direct_helpcode(&mut request);
        if !request.valid {
            return Vec::new();
        }
        self.registry.query(&request)
    }

    /// 同一个请求关掉句中辅助码的筛选再查一遍。
    pub fn query_without_syllable_helpcodes(&mut self) -> Vec<Item> {
        if !self.state.request.valid || self.state.request.syllable_helpcodes.is_empty() || !self.state.request.enable_mid_sentence_helpcode {
            return self.state.candidates.clone();
        }
        // 反引号段在请求里已换成分隔符，切分与不敲辅助码时一致；关掉开关，词典层就不再按约束筛。
        let mut request = self.state.request.clone();
        request.enable_mid_sentence_helpcode = false;
        self.registry.query(&request)
    }

    // ------------------------------------------------------------------------------------------
    // 刷新候选
    // ------------------------------------------------------------------------------------------

    fn apply_request_options(&self, request: &mut QueryRequest) {
        use crate::autocorrect::{DELETION, INSERTION, NEIGHBOR, TRANSPOSITION};
        request.enable_shuangpin_helpcode = self.enable_shuangpin_helpcode;
        request.enable_mid_sentence_helpcode = self.enable_mid_sentence_helpcode;
        if self.enable_direct_helpcode && request.scheme == SchemeType::Shuangpin {
            // 直接辅助码接管辅码：末尾单码/双码那套判断会把辅码字母当成别的东西，关掉；约束由解析器改写请求时再打开。
            request.enable_shuangpin_helpcode = false;
            request.enable_mid_sentence_helpcode = false;
            request.enable_direct_helpcode = true;
            request.direct_helpcode_slash_marker = self.direct_helpcode_slash_marker;
            request.direct_helpcode_uppercase_marker = self.direct_helpcode_uppercase_marker;
        }
        request.enable_quanpin_helpcode = self.enable_quanpin_helpcode;
        request.enable_quanpin_autocorrect_transposition = self.quanpin_autocorrect_types & TRANSPOSITION != 0;
        request.enable_quanpin_autocorrect_neighbor = self.quanpin_autocorrect_types & NEIGHBOR != 0;
        request.enable_quanpin_autocorrect_deletion = self.quanpin_autocorrect_types & DELETION != 0;
        request.enable_quanpin_autocorrect_insertion = self.quanpin_autocorrect_types & INSERTION != 0;
        request.fuzzy_pinyin = self.fuzzy_pinyin;
        request.sentence_association = self.sentence_association.clone();
        request.enabled_domain_dictionaries = self.enabled_domain_dictionaries.clone();
        request.rescoring_context = self.rescoring_context.clone();
    }

    fn resolve_direct_helpcode(&mut self, request: &mut QueryRequest) {
        if self.enable_direct_helpcode && request.valid && request.scheme == SchemeType::Shuangpin {
            self.registry.pinyin.resolve_direct_helpcode(request);
        }
    }

    fn refresh_candidates(&mut self) {
        self.state.preedit = self.scheme.inner().preedit();
        let mut request = self.scheme.inner().build_request();
        self.apply_request_options(&mut request);
        apply_shuangpin_helpcode_segmentation(&mut request, self.profile);
        self.resolve_direct_helpcode(&mut request);
        self.state.request = request;

        if !self.state.request.valid {
            self.state.candidates.clear();
            return;
        }
        let request = self.state.request.clone();
        self.state.candidates = self.registry.query(&request);
        if !matches!(self.scheme, Scheme::Wubi(_)) {
            return;
        }

        // 五笔提供者也会返回以输入为前缀的更长编码（逐键提示），一到三码的前缀几乎总有。只有恰好是这个码的行才说明码表
        // 答了；只有提示不能让混输放弃拼音。
        let wubi_table_answered = self.state.candidates.iter().any(|item| item.pinyin == request.normalized_input);
        // 码表答不出手上的码时，混输让组合长过四码，完整的拼写才能打完；码表答得出的码保持四码上限。
        let extended = self.wubi_options.mixed_pinyin && !wubi_table_answered;
        if let Scheme::Wubi(wubi) = &mut self.scheme {
            wubi.set_extended_length_allowed(extended);
        }
        if !self.wubi_options.mixed_pinyin {
            return;
        }

        // 五笔拼音混输：同一串字母同时交给五笔码表和全拼，五笔候选在前、拼音候选追加在后，两个表都答得出的词只留五笔那份。
        let mut pinyin = QuanpinScheme::default();
        pinyin.set_autocorrect_types(self.quanpin_autocorrect_types);
        pinyin.set_raw_input(&request.raw_input, &request.raw_input_with_cases);
        let mut pinyin_request = pinyin.build_request();
        self.apply_request_options(&mut pinyin_request);
        // 同样的物理按键敲出了这些字母，按键记录一并带过去，别让提供者拿到空的。
        pinyin_request.key_strokes = request.key_strokes.clone();
        if !pinyin_request.valid {
            return;
        }
        let pinyin_candidates = self.registry.query(&pinyin_request);
        if pinyin_candidates.is_empty() {
            return;
        }

        // 通配与混输同时开启时，含 z 的编码既可能是通配猜码也可能是拼音拼写（zi、zai、zhong）。通配查询按权重返回整页无关码行，
        // 放在前面会把拼音的正解挤到后几页，所以这种组合里拼音在前、通配结果在后；同词只留前面那份。
        let mut leading = std::mem::take(&mut self.state.candidates);
        let mut trailing = pinyin_candidates;
        if request.wubi_z_wildcard {
            std::mem::swap(&mut leading, &mut trailing);
        }
        let mut seen_words: HashSet<String> = leading.iter().map(|item| item.word.clone()).collect();
        for item in trailing {
            if seen_words.insert(item.word.clone()) {
                leading.push(item);
            }
        }
        self.state.candidates = leading;
    }

    /// 换了辅助码表之后刷新候选。
    pub fn set_helpcode_keymap(&mut self, keymap: crate::helpcode::Keymap) {
        self.registry.pinyin.set_helpcode_keymap(keymap);
        self.refresh_candidates();
    }
}
