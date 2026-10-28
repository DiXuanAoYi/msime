mod behavior;
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
    sprite: character::Sprite,
}

impl App {
    fn new(cc: &eframe::CreationContext<'_>, demo: bool) -> Self {
        let ctx = cc.egui_ctx.clone();
        let feed = feed::spawn(move || ctx.request_repaint());
        let seed = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos())
            .unwrap_or(1);
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

            let response = ui.interact(body, ui.id().with("companion"), Sense::click());
            if response.clicked() {
                self.companion.pat();
            }
            response.context_menu(|ui| {
                if ui.button("摸摸头").clicked() {
                    self.companion.pat();
                    ui.close_menu();
                }
                if ui.button("退出").clicked() {
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                }
            });
            if pose == Pose::Listen {
                painter.text(
                    pos2(feet.x, feet.y - HEIGHT - 26.0),
                    egui::Align2::CENTER_CENTER,
                    "在听…",
                    egui::FontId::proportional(13.0),
                    Color32::from_white_alpha(200),
                );
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

/// 默认用内置的大肥鱼；`ZHIYU_COMPANION_SPRITE` 指向别的 PNG（例如另一款皮肤的 character.png）就换成它。
fn load_sprite(ctx: &egui::Context) -> character::Sprite {
    if let Some(path) = std::env::var_os("ZHIYU_COMPANION_SPRITE") {
        if let Ok(bytes) = std::fs::read(&path) {
            if let Ok(sprite) = character::Sprite::load(ctx, &bytes) {
                return sprite;
            }
        }
    }
    character::Sprite::load(ctx, character::BIGFISH).expect("内置立绘损坏")
}

fn main() -> eframe::Result {
    let demo = std::env::args().any(|a| a == "--demo");
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("知语伴侣")
            .with_transparent(true)
            .with_decorations(false)
            .with_always_on_top()
            .with_taskbar(false)
            .with_mouse_passthrough(true)
            .with_inner_size([1280.0, 720.0])
            .with_position([0.0, 0.0]),
        ..Default::default()
    };
    eframe::run_native("zhiyu-companion", options, Box::new(move |cc| Ok(Box::new(App::new(cc, demo)))))
}
