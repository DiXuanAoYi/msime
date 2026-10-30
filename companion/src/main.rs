mod behavior;
mod bond;
mod pack;
mod settings;
mod character;
mod feed;

use behavior::{Companion, Input, Pose, Rect, HEIGHT};
use eframe::egui::{self, pos2, vec2, Color32, Sense, Stroke};
use std::time::Instant;

/// 没有输入法在跑时，也能自己演一遍：`zhiyu-companion --demo`。
fn demo_input(t: f32, w: f32, h: f32) -> Input {
    let window = Rect { x: w * 0.35, y: h * 0.55, w: 360.0, h: 64.0 };
    match (t % 18.0) as u32 {
        0..=5 => Input::default(),
        6..=11 => Input { candidate: Some(window), listening: false },
        12..=14 => Input { candidate: None, listening: true },
        _ => Input::default(),
    }
}

struct App {
    feed: feed::Shared,
    companion: Companion,
    started: Instant,
    last: Instant,
    demo: bool,
    sized: bool,
    passthrough: bool,
    screen: (f32, f32),
    sprite: character::Sprites,
    bond: bond::Bond,
    bond_path: Option<std::path::PathBuf>,
    /// 摸头、喂食后飘在头上的小字和剩余秒数。
    pop: Option<(String, f32)>,
    settings: settings::Settings,
    settings_path: Option<std::path::PathBuf>,
    applied_top: Option<bool>,
    last_pose: Pose,
    bubble: Option<(&'static str, f32)>,
    placed: bool,
    drag_save: f32,
}

impl App {
    fn new(cc: &eframe::CreationContext<'_>, demo: bool) -> Self {
        let ctx = cc.egui_ctx.clone();
        let feed = feed::spawn(move || ctx.request_repaint());
        let seed = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos())
            .unwrap_or(1);
        let settings_path = settings::default_path();
        let settings = settings_path.as_deref().map(settings::Settings::load).unwrap_or_default();
        let bond_path = bond::default_path();
        let bond = bond_path.as_deref().map(bond::Bond::load).unwrap_or_default();
        Self {
            feed,
            companion: Companion::new(1280.0, 700.0, seed),
            started: Instant::now(),
            last: Instant::now(),
            demo,
            sized: false,
            passthrough: true,
            screen: (1280.0, 720.0),
            sprite: load_sprite(&cc.egui_ctx),
            bond,
            bond_path,
            pop: None,
            settings,
            settings_path,
            applied_top: None,
            last_pose: Pose::Stand,
            bubble: None,
            placed: false,
            drag_save: 0.0,
        }
    }

    fn save_settings(&self) {
        if let Some(path) = &self.settings_path {
            self.settings.save(path);
        }
    }

    fn save_bond(&self) {
        if let Some(path) = &self.bond_path {
            self.bond.save(path);
        }
    }

    /// 摸头和喂食共用：她总会开心，涨没涨好感由冷却决定。
    fn react(&mut self, level_before: u32, gained: Option<u32>) {
        self.companion.pat();
        if let Some(points) = gained {
            let leveled = self.bond.level() > level_before;
            self.pop = Some((if leveled { format!("+{points} 升级啦") } else { format!("+{points}") }, 1.4));
            self.save_bond();
        }
    }

    /// 透明覆盖窗盖满主屏；返回屏幕大小和任务栏上沿（都是逻辑点）。
    fn fit_screen(&mut self, ctx: &egui::Context) {
        let ppp = ctx.pixels_per_point();
        let Some(monitor) = ctx.input(|i| i.viewport().monitor_size) else { return };
        let work_bottom = work_area_bottom().map(|b| b / ppp).unwrap_or(monitor.y - 48.0);
        if !self.sized || self.screen != (monitor.x, monitor.y) {
            self.sized = true;
            self.screen = (monitor.x, monitor.y);
            ctx.send_viewport_cmd(egui::ViewportCommand::OuterPosition(pos2(0.0, 0.0)));
            ctx.send_viewport_cmd(egui::ViewportCommand::InnerSize(monitor));
        }
        self.companion.resize(monitor.x, work_bottom);
    }
}

impl eframe::App for App {
    fn clear_color(&self, _visuals: &egui::Visuals) -> [f32; 4] {
        [0.0, 0.0, 0.0, 0.0]
    }

    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.fit_screen(ctx);
        let now = Instant::now();
        let dt = (now - self.last).as_secs_f32().min(0.1);
        self.last = now;
        self.bond.tick(dt);
        if let Some((_, left)) = &mut self.pop {
            *left -= dt;
        }
        if self.pop.as_ref().is_some_and(|(_, left)| *left <= 0.0) {
            self.pop = None;
        }
        let t = self.started.elapsed().as_secs_f32();
        let ppp = ctx.pixels_per_point();

        let input = if self.demo {
            demo_input(t, self.screen.0, self.screen.1)
        } else {
            let state = self.feed.lock().map(|s| *s).unwrap_or_default();
            Input {
                candidate: state.candidate.map(|r| Rect { x: r.x / ppp, y: r.y / ppp, w: r.w / ppp, h: r.h / ppp }),
                listening: state.recording,
            }
        };
        self.companion.update(dt, &input);
        let pose = self.companion.pose(input.listening);
        if !self.placed {
            self.placed = true;
            self.companion.set_x(self.settings.x_ratio * self.screen.0);
        }
        if self.applied_top != Some(self.settings.always_on_top) {
            self.applied_top = Some(self.settings.always_on_top);
            let level = if self.settings.always_on_top { egui::WindowLevel::AlwaysOnTop } else { egui::WindowLevel::Normal };
            ctx.send_viewport_cmd(egui::ViewportCommand::WindowLevel(level));
        }
        if pose != self.last_pose {
            let text = match (self.last_pose, pose) {
                (Pose::Sleep, Pose::Happy) => Some("嘿嘿，醒啦"),
                (Pose::Sleep, _) => Some("嗯？醒啦"),
                (_, Pose::Perch) => Some("我陪你写"),
                (_, Pose::Sleep) => Some("Zzz…"),
                _ => None,
            };
            self.bubble = text.map(|t| (t, 2.2));
            self.last_pose = pose;
        }
        if let Some((_, left)) = &mut self.bubble {
            *left -= dt;
        }
        if self.bubble.as_ref().is_some_and(|(_, left)| *left <= 0.0) {
            self.bubble = None;
        }

        // 鼠标不在她身上时让点击穿过去，不挡住别的软件。
        let feet = pos2(self.companion.pos.0, self.companion.pos.1);
        let body = egui::Rect::from_min_max(pos2(feet.x - 50.0, feet.y - HEIGHT - 8.0), pos2(feet.x + 50.0, feet.y + 6.0));
        let over = cursor_point().map(|(x, y)| body.contains(pos2(x / ppp, y / ppp))).unwrap_or(false);
        if over == self.passthrough {
            self.passthrough = !over;
            ctx.send_viewport_cmd(egui::ViewportCommand::MousePassthrough(self.passthrough));
        }

        egui::CentralPanel::default().frame(egui::Frame::NONE).show(ctx, |ui| {
            let painter = ui.painter().clone();
            if self.demo {
                if let Some(w) = input.candidate {
                    let r = egui::Rect::from_min_size(pos2(w.x, w.y), vec2(w.w, w.h));
                    painter.rect(r, 8.0, Color32::from_rgb(40, 44, 52), Stroke::new(1.0, Color32::GRAY), egui::StrokeKind::Inside);
                    painter.text(r.center(), egui::Align2::CENTER_CENTER, "1 你好  2 泥号  3 拟好", egui::FontId::proportional(18.0), Color32::WHITE);
                }
            }
            character::draw(&painter, &self.sprite, feet, self.companion.facing, pose, t);

            let response = ui.interact(body, ui.id().with("companion"), Sense::click_and_drag());
            if response.dragged() {
                if let Some(p) = response.interact_pointer_pos() {
                    if self.companion.drag_to(p.x) {
                        self.drag_save = 0.6;
                    }
                }
            }
            if self.drag_save > 0.0 && !response.dragged() {
                self.drag_save -= dt;
                if self.drag_save <= 0.0 && self.screen.0 > 0.0 {
                    self.settings.x_ratio = (self.companion.pos.0 / self.screen.0).clamp(0.0, 1.0);
                    self.save_settings();
                }
            }
            if response.clicked() {
                let before = self.bond.level();
                let gained = self.bond.pat().then_some(1);
                self.react(before, gained);
            }
            response.context_menu(|ui| {
                let (level, into, need) = self.bond.progress();
                ui.label(format!("好感度 Lv.{level}（{into}/{need}）"));
                ui.separator();
                if ui.button("摸摸头").clicked() {
                    let before = self.bond.level();
                    let gained = self.bond.pat().then_some(1);
                    self.react(before, gained);
                    ui.close_menu();
                }
                if ui.add_enabled(self.bond.feed_ready(), egui::Button::new("喂小鱼干")).clicked() {
                    let before = self.bond.level();
                    let gained = self.bond.feed().then_some(3);
                    self.react(before, gained);
                    ui.close_menu();
                }
                ui.separator();
                let mut changed = false;
                changed |= ui.checkbox(&mut self.settings.always_on_top, "置顶").changed();
                changed |= ui.checkbox(&mut self.settings.bubbles, "说话气泡").changed();
                if changed {
                    self.save_settings();
                }
                ui.separator();
                if ui.button("退出").clicked() {
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                }
            });
            if let Some((text, left)) = &self.pop {
                let rise = (1.4 - left) * 18.0;
                painter.text(
                    pos2(feet.x + 28.0, feet.y - HEIGHT - 10.0 - rise),
                    egui::Align2::CENTER_CENTER,
                    format!("♥ {text}"),
                    egui::FontId::proportional(14.0),
                    Color32::from_rgba_unmultiplied(255, 110, 150, (255.0 * (left / 1.4).clamp(0.0, 1.0)) as u8),
                );
            }
            if self.settings.bubbles {
                let say = if pose == Pose::Listen { Some("在听…") } else { self.bubble.map(|(t, _)| t) };
                if let Some(text) = say {
                    painter.text(
                        pos2(feet.x, feet.y - HEIGHT - 26.0),
                        egui::Align2::CENTER_CENTER,
                        text,
                        egui::FontId::proportional(13.0),
                        Color32::from_white_alpha(220),
                    );
                }
            }
        });
        ctx.request_repaint_after(std::time::Duration::from_millis(16));
    }
}

#[cfg(windows)]
fn work_area_bottom() -> Option<f32> {
    use windows_sys::Win32::Foundation::RECT;
    use windows_sys::Win32::UI::WindowsAndMessaging::{SystemParametersInfoW, SPI_GETWORKAREA};
    let mut rect = RECT { left: 0, top: 0, right: 0, bottom: 0 };
    let ok = unsafe { SystemParametersInfoW(SPI_GETWORKAREA, 0, &mut rect as *mut _ as *mut _, 0) };
    (ok != 0).then_some(rect.bottom as f32)
}

#[cfg(not(windows))]
fn work_area_bottom() -> Option<f32> {
    None
}

#[cfg(windows)]
fn cursor_point() -> Option<(f32, f32)> {
    use windows_sys::Win32::Foundation::POINT;
    use windows_sys::Win32::UI::WindowsAndMessaging::GetCursorPos;
    let mut p = POINT { x: 0, y: 0 };
    (unsafe { GetCursorPos(&mut p) } != 0).then_some((p.x as f32, p.y as f32))
}

#[cfg(not(windows))]
fn cursor_point() -> Option<(f32, f32)> {
    None
}

/// 默认用内置的大肥鱼；`ZHIYU_COMPANION_PET` 指向宠物包目录（见 `pack.rs`）就换成它；`ZHIYU_COMPANION_SPRITE` 指向别的 PNG（例如另一款皮肤的 character.png）就换成它。
fn load_sprite(ctx: &egui::Context) -> character::Sprites {
    if let Some(dir) = std::env::var_os("ZHIYU_COMPANION_PET") {
        match pack::load(std::path::Path::new(&dir)) {
            Ok(pack) => {
                if let Ok(full) = character::Sprite::load(ctx, &pack.full) {
                    let peek_bytes = pack.peek.as_deref().unwrap_or(&pack.full);
                    if let Ok(peek) = character::Sprite::load(ctx, peek_bytes) {
                        return character::Sprites { full, peek };
                    }
                }
                eprintln!("宠物包「{}」的立绘解码失败，改用内置的大肥鱼", pack.name);
            }
            Err(e) => eprintln!("宠物包加载失败（{e}），改用内置的大肥鱼"),
        }
    }
    if let Some(path) = std::env::var_os("ZHIYU_COMPANION_SPRITE") {
        if let Ok(bytes) = std::fs::read(&path) {
            if let Ok(full) = character::Sprite::load(ctx, &bytes) {
                let peek = character::Sprite::load(ctx, &bytes).expect("同一张图不会解码失败");
                return character::Sprites { full, peek };
            }
        }
    }
    character::Sprites {
        full: character::Sprite::load(ctx, character::BIGFISH).expect("内置立绘损坏"),
        peek: character::Sprite::load(ctx, character::BIGFISH_PEEK).expect("内置探头立绘损坏"),
    }
}

fn main() -> eframe::Result {
    let demo = std::env::args().any(|a| a == "--demo");
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("知语伴侣")
            .with_transparent(true)
            .with_decorations(false)
                        .with_taskbar(false)
            .with_mouse_passthrough(true)
            .with_inner_size([1280.0, 720.0])
            .with_position([0.0, 0.0]),
        ..Default::default()
    };
    eframe::run_native("zhiyu-companion", options, Box::new(move |cc| Ok(Box::new(App::new(cc, demo)))))
}
