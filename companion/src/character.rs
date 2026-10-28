//! 用输入法皮肤里的「大肥鱼」立绘当伴侣。
//!
//! 现在只有一张静态立绘，所以动作靠整体变形做：呼吸、摇摆、蹦跳、压扁；“双手按在耳朵上”这类需要
//! 改变肢体的动作，先在立绘上叠一对手臂。等有了分姿势的画稿（站立、听、开心……），
//! 只要在 `Sprite` 里按姿势换图，`draw` 的其余部分不用动。

use crate::behavior::{Pose, HEIGHT};
use eframe::egui::{
    self, epaint::Vertex, pos2, Color32, Context, Mesh, Painter, Pos2, Shape, Stroke, TextureHandle, TextureOptions, Vec2,
};

const SLEEVE: Color32 = Color32::from_rgb(30, 36, 82);
const SKIN: Color32 = Color32::from_rgb(255, 228, 210);
const INK: Color32 = Color32::from_rgb(52, 40, 84);

/// 内置的大肥鱼（来自 `skins/bigfish`，随仓库同一授权）。
pub const BIGFISH: &[u8] = include_bytes!("../assets/bigfish.png");

pub struct Sprite {
    texture: TextureHandle,
    /// 宽 / 高。
    aspect: f32,
}

impl Sprite {
    pub fn load(ctx: &Context, png: &[u8]) -> Result<Self, String> {
        let image = image::load_from_memory(png).map_err(|e| e.to_string())?.to_rgba8();
        let size = [image.width() as usize, image.height() as usize];
        let aspect = size[0] as f32 / size[1] as f32;
        let color = egui::ColorImage::from_rgba_unmultiplied(size, image.as_raw());
        Ok(Self { texture: ctx.load_texture("companion-sprite", color, TextureOptions::LINEAR), aspect })
    }
}

/// 一帧的整体变形。
struct Motion {
    sx: f32,
    sy: f32,
    angle: f32,
    lift: f32,
}

fn motion(pose: Pose, t: f32) -> Motion {
    let breathe = (t * 2.2).sin();
    match pose {
        Pose::Stand => Motion { sx: 1.0 - 0.005 * breathe, sy: 1.0 + 0.012 * breathe, angle: 0.0, lift: 0.0 },
        Pose::Walk => Motion { sx: 1.0, sy: 1.0, angle: (t * 9.0).sin() * 0.05, lift: (t * 9.0).sin().abs() * 4.0 },
        Pose::Sit => Motion { sx: 1.07, sy: 0.86 + 0.01 * breathe, angle: 0.0, lift: 0.0 },
        Pose::Perch => Motion { sx: 1.0, sy: 1.0 + 0.01 * breathe, angle: (t * 1.8).sin() * 0.03, lift: breathe * 1.5 },
        Pose::Jump => Motion { sx: 0.94, sy: 1.08, angle: 0.1, lift: 0.0 },
        Pose::Listen => Motion { sx: 1.0, sy: 1.0 + 0.008 * breathe, angle: 0.07, lift: 0.0 },
        Pose::Happy => Motion { sx: 1.0, sy: 1.0, angle: (t * 8.0).sin() * 0.06, lift: (t * 8.0).sin().abs() * 8.0 },
    }
}

/// 脚底中心 `feet`，`facing` 为 1 保持原图朝向、-1 左右镜像，`t` 是秒。
pub fn draw(painter: &Painter, sprite: &Sprite, feet: Pos2, facing: f32, pose: Pose, t: f32) {
    let m = motion(pose, t);
    let h = HEIGHT;
    let w = h * sprite.aspect;
    let (sin, cos) = m.angle.sin_cos();
    // 立绘局部坐标（原点在脚底中心，y 向下为正）→ 屏幕。
    let to_screen = |lx: f32, ly: f32| {
        let (x, y) = (lx * m.sx, ly * m.sy);
        let (xr, yr) = (x * cos - y * sin, x * sin + y * cos);
        pos2(feet.x + xr * facing, feet.y + yr - m.lift)
    };
    // 立绘上某个位置（占整张图宽高的比例，左上角为 0,0）。
    let on_art = |fx: f32, fy: f32| to_screen((fx - 0.5) * w, -(1.0 - fy) * h);

    if !matches!(pose, Pose::Jump | Pose::Perch) {
        painter.add(Shape::ellipse_filled(
            pos2(feet.x, feet.y + 1.0),
            Vec2::new(w * 0.32, 5.0),
            Color32::from_black_alpha(46),
        ));
    }

    let mut mesh = Mesh::with_texture(sprite.texture.id());
    for (lx, ly, u, v) in [(-0.5, -1.0, 0.0, 0.0), (0.5, -1.0, 1.0, 0.0), (0.5, 0.0, 1.0, 1.0), (-0.5, 0.0, 0.0, 1.0)] {
        mesh.vertices.push(Vertex { pos: to_screen(lx * w, ly * h), uv: pos2(u, v), color: Color32::WHITE });
    }
    mesh.indices.extend([0, 1, 2, 0, 2, 3]);
    painter.add(Shape::mesh(mesh));

    match pose {
        Pose::Listen => hands_on_ears(painter, &on_art, w, t),
        Pose::Happy => {
            let rise = (t * 2.5).fract();
            for (dx, delay) in [(-0.12, 0.0), (0.1, 0.4)] {
                let p = (rise + delay).fract();
                let at = on_art(0.5 + dx, -0.02 - p * 0.18);
                painter.text(
                    at,
                    egui::Align2::CENTER_CENTER,
                    "♥",
                    egui::FontId::proportional(16.0),
                    Color32::from_rgba_unmultiplied(255, 110, 150, (255.0 * (1.0 - p)) as u8),
                );
            }
        }
        _ => {}
    }
}

/// 双手按在耳朵（头两侧的鱼鳍）上，旁边冒几道声波。
fn hands_on_ears(painter: &Painter, on_art: &dyn Fn(f32, f32) -> Pos2, w: f32, t: f32) {
    let arm = (w * 0.05).max(5.0);
    for (shoulder, elbow, hand, outward) in [
        ((0.31, 0.63), (0.16, 0.60), (0.11, 0.45), -1.0f32),
        ((0.69, 0.63), (0.84, 0.60), (0.89, 0.46), 1.0f32),
    ] {
        let (s, e, p) = (on_art(shoulder.0, shoulder.1), on_art(elbow.0, elbow.1), on_art(hand.0, hand.1));
        painter.line_segment([s, e], Stroke::new(arm, SLEEVE));
        painter.line_segment([e, p], Stroke::new(arm, SLEEVE));
        painter.circle_filled(e, arm * 0.5, SLEEVE);
        painter.circle(p, w * 0.058, SKIN, Stroke::new(1.2, INK));
        let pulse = (t * 5.0).sin() * 0.5 + 0.5;
        for k in 0..2 {
            let r = 7.0 + k as f32 * 6.0;
            let alpha = (170.0 * (1.0 - k as f32 * 0.4) * (0.45 + pulse * 0.55)) as u8;
            let centre = pos2(p.x + outward * (w * 0.07), p.y);
            let arc: Vec<Pos2> = (0..=10)
                .map(|i| {
                    let a = -0.9 + 1.8 * i as f32 / 10.0;
                    pos2(centre.x + outward * r * a.cos(), centre.y + r * a.sin())
                })
                .collect();
            painter.add(Shape::line(arc, Stroke::new(1.8, Color32::from_white_alpha(alpha))));
        }
    }
}
