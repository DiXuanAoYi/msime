//! 伴侣的行为：不碰界面，只根据“候选窗在哪、是不是在录音”算出她在哪、摆什么姿势。
//! 坐标都是逻辑点，原点在屏幕左上角，`pos` 是脚底中心。

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

/// 外界告诉她的事。
#[derive(Debug, Clone, Copy, Default)]
pub struct Input {
    /// 候选窗可见时的位置。
    pub candidate: Option<Rect>,
    /// 语音正在录音。
    pub listening: bool,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Pose {
    Stand,
    Walk,
    Sit,
    /// 站在候选窗上沿，看着你打字。
    Perch,
    Jump,
    /// 双手按在耳朵上，认真听。
    Listen,
    /// 被摸头的开心样子。
    Happy,
    /// 很久没人理她，趴着打盹。
    Sleep,
}

#[derive(Debug, Clone, Copy)]
enum Mode {
    Floor(FloorAction),
    Jump { from: (f32, f32), to: (f32, f32), t: f32, dur: f32 },
    Perched { grace: f32 },
}

#[derive(Debug, Clone, Copy)]
enum FloorAction {
    Stand(f32),
    Walk { to: f32 },
    Sit(f32),
}

pub struct Companion {
    pub pos: (f32, f32),
    /// 1 朝右，-1 朝左。
    pub facing: f32,
    mode: Mode,
    happy: f32,
    /// 距上次有动静（候选窗、录音、摸头）过了多久。
    idle: f32,
    seed: u32,
    screen_w: f32,
    floor_y: f32,
}

/// 候选窗消失后她还站多久。连续打字时候选窗在每次上屏时会闪一下，等一会儿再下去，不然她会一直蹦。
pub const PERCH_GRACE: f32 = 2.5;
const WALK_SPEED: f32 = 55.0;
const FOLLOW_RATE: f32 = 14.0;
pub const HEIGHT: f32 = 140.0;
/// 这么久没人理她就趴下打盹，有动静立刻醒。
pub const SLEEP_AFTER: f32 = 300.0;

/// 站在候选窗上沿偏右的位置；窗太靠上站不下时，站到它左边。
pub fn perch_point(window: Rect) -> (f32, f32) {
    if window.y < HEIGHT + 8.0 {
        return (window.x - 30.0, window.y + window.h);
    }
    (window.x + window.w - (window.w * 0.18).clamp(24.0, 70.0), window.y)
}

impl Companion {
    pub fn new(screen_w: f32, floor_y: f32, seed: u32) -> Self {
        Self {
            pos: (screen_w * 0.8, floor_y),
            facing: -1.0,
            mode: Mode::Floor(FloorAction::Stand(2.0)),
            happy: 0.0,
            idle: 0.0,
            seed: seed.max(1),
            screen_w,
            floor_y,
        }
    }

    pub fn resize(&mut self, screen_w: f32, floor_y: f32) {
        self.screen_w = screen_w;
        self.floor_y = floor_y;
        self.pos.0 = self.pos.0.clamp(40.0, (screen_w - 40.0).max(40.0));
        if matches!(self.mode, Mode::Floor(_)) {
            self.pos.1 = floor_y;
        }
    }

    pub fn pat(&mut self) {
        self.happy = 1.6;
        self.idle = 0.0;
    }

    pub fn is_asleep(&self) -> bool {
        self.idle >= SLEEP_AFTER && matches!(self.mode, Mode::Floor(_))
    }

    fn random(&mut self) -> f32 {
        // xorshift32，够用，而且测试里结果固定。
        let mut x = self.seed;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        self.seed = x;
        (x >> 8) as f32 / (1u32 << 24) as f32
    }

    pub fn pose(&self, listening: bool) -> Pose {
        if self.happy > 0.0 {
            return Pose::Happy;
        }
        if self.is_asleep() && !listening {
            return Pose::Sleep;
        }
        match self.mode {
            Mode::Jump { .. } => Pose::Jump,
            Mode::Perched { .. } if listening => Pose::Listen,
            Mode::Perched { .. } => Pose::Perch,
            Mode::Floor(_) if listening => Pose::Listen,
            Mode::Floor(FloorAction::Walk { .. }) => Pose::Walk,
            Mode::Floor(FloorAction::Sit(_)) => Pose::Sit,
            Mode::Floor(FloorAction::Stand(_)) => Pose::Stand,
        }
    }

    pub fn is_on_floor(&self) -> bool {
        matches!(self.mode, Mode::Floor(_))
    }

    pub fn update(&mut self, dt: f32, input: &Input) {
        self.happy = (self.happy - dt).max(0.0);
        if input.candidate.is_some() || input.listening {
            self.idle = 0.0;
        } else {
            self.idle += dt;
        }
        let target = input.candidate.map(perch_point);
        match (self.mode, target) {
            // 有候选窗：不在上面就跳上去，已经在上面就跟着它走。
            (Mode::Floor(_) | Mode::Perched { .. }, Some(to)) if !self.is_perched_at(to) => {
                self.start_jump(to, 0.45);
            }
            (Mode::Perched { .. }, Some(to)) => {
                self.mode = Mode::Perched { grace: PERCH_GRACE };
                let k = 1.0 - (-FOLLOW_RATE * dt).exp();
                self.pos.0 += (to.0 - self.pos.0) * k;
                self.pos.1 += (to.1 - self.pos.1) * k;
            }
            (Mode::Jump { from, t, dur, .. }, Some(new_to)) => {
                self.mode = Mode::Jump { from, to: new_to, t, dur };
            }
            (Mode::Perched { grace }, None) => {
                let grace = grace - dt;
                self.mode = Mode::Perched { grace };
                if grace <= 0.0 {
                    let x = self.pos.0.clamp(40.0, (self.screen_w - 40.0).max(40.0));
                    self.start_jump((x, self.floor_y), 0.55);
                }
            }
            _ => {}
        }
        match self.mode {
            Mode::Jump { from, to, t, dur } => {
                let t = t + dt;
                let p = (t / dur).min(1.0);
                let ease = p * p * (3.0 - 2.0 * p);
                self.pos.0 = from.0 + (to.0 - from.0) * ease;
                let arc = (std::f32::consts::PI * p).sin() * 50.0;
                self.pos.1 = from.1 + (to.1 - from.1) * ease - arc;
                self.facing = if to.0 >= from.0 { 1.0 } else { -1.0 };
                if p >= 1.0 {
                    self.pos = to;
                    self.mode = if input.candidate.is_some() {
                        Mode::Perched { grace: PERCH_GRACE }
                    } else {
                        Mode::Floor(FloorAction::Stand(1.5))
                    };
                } else {
                    self.mode = Mode::Jump { from, to, t, dur };
                }
            }
            Mode::Floor(action) => self.update_floor(dt, action, input.listening),
            Mode::Perched { .. } => {}
        }
    }

    fn is_perched_at(&self, to: (f32, f32)) -> bool {
        matches!(self.mode, Mode::Perched { .. }) && (self.pos.0 - to.0).abs() < 160.0 && (self.pos.1 - to.1).abs() < 120.0
    }

    fn start_jump(&mut self, to: (f32, f32), dur: f32) {
        let far = ((to.0 - self.pos.0).powi(2) + (to.1 - self.pos.1).powi(2)).sqrt();
        self.mode = Mode::Jump { from: self.pos, to, t: 0.0, dur: dur + (far / 2400.0).min(0.4) };
    }

    fn update_floor(&mut self, dt: f32, action: FloorAction, listening: bool) {
        self.pos.1 = self.floor_y;
        // 听你说话时不乱走；睡着了也不走。
        if listening || self.idle >= SLEEP_AFTER {
            return;
        }
        self.mode = Mode::Floor(match action {
            FloorAction::Stand(left) | FloorAction::Sit(left) if left > dt => match action {
                FloorAction::Sit(_) => FloorAction::Sit(left - dt),
                _ => FloorAction::Stand(left - dt),
            },
            FloorAction::Stand(_) | FloorAction::Sit(_) => {
                let r = self.random();
                if r < 0.55 {
                    let to = 40.0 + self.random() * (self.screen_w - 80.0).max(1.0);
                    FloorAction::Walk { to }
                } else if r < 0.8 {
                    FloorAction::Sit(3.0 + self.random() * 5.0)
                } else {
                    FloorAction::Stand(2.0 + self.random() * 4.0)
                }
            }
            FloorAction::Walk { to } => {
                let dx = to - self.pos.0;
                if dx.abs() < 3.0 {
                    FloorAction::Stand(1.5 + self.random() * 3.0)
                } else {
                    self.facing = dx.signum();
                    self.pos.0 += dx.signum() * WALK_SPEED * dt;
                    FloorAction::Walk { to }
                }
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn window() -> Rect {
        Rect { x: 600.0, y: 500.0, w: 360.0, h: 70.0 }
    }

    fn run(c: &mut Companion, seconds: f32, input: &Input) {
        let mut t = 0.0;
        while t < seconds {
            c.update(1.0 / 60.0, input);
            t += 1.0 / 60.0;
        }
    }

    #[test]
    fn jumps_onto_the_candidate_window_when_typing_starts() {
        let mut c = Companion::new(1920.0, 1000.0, 7);
        let input = Input { candidate: Some(window()), listening: false };
        c.update(1.0 / 60.0, &input);
        assert_eq!(c.pose(false), Pose::Jump);
        run(&mut c, 1.5, &input);
        assert_eq!(c.pose(false), Pose::Perch);
        let (x, y) = perch_point(window());
        assert!((c.pos.0 - x).abs() < 1.0 && (c.pos.1 - y).abs() < 1.0);
    }

    #[test]
    fn follows_the_window_when_it_moves() {
        let mut c = Companion::new(1920.0, 1000.0, 7);
        let mut input = Input { candidate: Some(window()), listening: false };
        run(&mut c, 1.5, &input);
        input.candidate = Some(Rect { x: 640.0, y: 520.0, ..window() });
        run(&mut c, 1.0, &input);
        let (x, y) = perch_point(input.candidate.unwrap());
        assert!((c.pos.0 - x).abs() < 1.0 && (c.pos.1 - y).abs() < 1.0);
        assert_eq!(c.pose(false), Pose::Perch);
    }

    #[test]
    fn stays_through_the_flicker_between_candidate_windows() {
        let mut c = Companion::new(1920.0, 1000.0, 7);
        let mut input = Input { candidate: Some(window()), listening: false };
        run(&mut c, 1.5, &input);
        input.candidate = None;
        run(&mut c, 1.0, &input);
        assert_eq!(c.pose(false), Pose::Perch, "窗口只是闪了一下，不该下来");
        input.candidate = Some(window());
        run(&mut c, 0.5, &input);
        assert_eq!(c.pose(false), Pose::Perch);
    }

    #[test]
    fn comes_back_down_after_typing_stops() {
        let mut c = Companion::new(1920.0, 1000.0, 7);
        let mut input = Input { candidate: Some(window()), listening: false };
        run(&mut c, 1.5, &input);
        input.candidate = None;
        run(&mut c, PERCH_GRACE + 2.0, &input);
        assert!(c.is_on_floor());
        assert!((c.pos.1 - 1000.0).abs() < 1.0);
    }

    #[test]
    fn puts_hands_on_ears_while_recording_and_stops_wandering() {
        let mut c = Companion::new(1920.0, 1000.0, 3);
        let input = Input { candidate: None, listening: true };
        let x = c.pos.0;
        run(&mut c, 10.0, &input);
        assert_eq!(c.pose(true), Pose::Listen);
        assert_eq!(c.pos.0, x);
        // 在候选窗上听也一样。
        let typing = Input { candidate: Some(window()), listening: true };
        run(&mut c, 1.5, &typing);
        assert_eq!(c.pose(true), Pose::Listen);
    }

    #[test]
    fn wanders_the_floor_when_nothing_is_happening() {
        let mut c = Companion::new(1920.0, 1000.0, 11);
        let start = c.pos.0;
        let mut moved = false;
        for _ in 0..40 {
            run(&mut c, 1.0, &Input::default());
            moved |= (c.pos.0 - start).abs() > 20.0;
            assert!(c.pos.0 >= 0.0 && c.pos.0 <= 1920.0);
            assert_eq!(c.pos.1, 1000.0);
        }
        assert!(moved);
    }

    #[test]
    fn window_near_the_top_of_the_screen_gets_a_spot_beside_it() {
        let top = Rect { x: 600.0, y: 40.0, w: 360.0, h: 70.0 };
        let (x, y) = perch_point(top);
        assert!(x < top.x && y > top.y);
    }

    #[test]
    fn dozes_off_when_ignored_and_wakes_on_any_activity() {
        let mut c = Companion::new(1920.0, 1000.0, 9);
        run(&mut c, SLEEP_AFTER - 5.0, &Input::default());
        assert_ne!(c.pose(false), Pose::Sleep);
        run(&mut c, 12.0, &Input::default());
        assert_eq!(c.pose(false), Pose::Sleep);
        let x = c.pos.0;
        run(&mut c, 30.0, &Input::default());
        assert_eq!(c.pos.0, x, "睡着了不该走动");
        c.pat();
        assert_ne!(c.pose(false), Pose::Sleep);
        run(&mut c, 12.0, &Input::default());
        assert_ne!(c.pose(false), Pose::Sleep, "被叫醒后计时重来");
        run(&mut c, SLEEP_AFTER + 1.0, &Input::default());
        assert_eq!(c.pose(false), Pose::Sleep);
        run(&mut c, 1.5, &Input { candidate: Some(window()), listening: false });
        assert_eq!(c.pose(false), Pose::Perch, "开始打字就醒了去候选窗");
    }

    #[test]
    fn recording_wakes_her_up_to_listen() {
        let mut c = Companion::new(1920.0, 1000.0, 9);
        run(&mut c, SLEEP_AFTER + 5.0, &Input::default());
        assert_eq!(c.pose(false), Pose::Sleep);
        run(&mut c, 0.1, &Input { candidate: None, listening: true });
        assert_eq!(c.pose(true), Pose::Listen);
    }

    #[test]
    fn patting_makes_her_happy_for_a_moment() {
        let mut c = Companion::new(1920.0, 1000.0, 5);
        c.pat();
        assert_eq!(c.pose(false), Pose::Happy);
        run(&mut c, 2.0, &Input::default());
        assert_ne!(c.pose(false), Pose::Happy);
    }
}
