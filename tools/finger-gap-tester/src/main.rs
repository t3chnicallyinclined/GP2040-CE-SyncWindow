mod app;
mod input;
mod stats;

use egui::Color32;

fn configure_style(ctx: &egui::Context) {
    let mut visuals = egui::Visuals::dark();
    visuals.panel_fill = Color32::from_rgb(18, 18, 24);
    visuals.window_fill = Color32::from_rgb(18, 18, 24);
    visuals.selection.bg_fill = Color32::from_rgb(0, 180, 216);
    visuals.hyperlink_color = Color32::from_rgb(0, 180, 216);
    ctx.set_visuals(visuals);
}

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([800.0, 600.0])
            .with_min_inner_size([640.0, 480.0])
            .with_title("NOBD Finger Gap Tester"),
        ..Default::default()
    };

    eframe::run_native(
        "NOBD Finger Gap Tester",
        options,
        Box::new(|cc| {
            configure_style(&cc.egui_ctx);
            Ok(Box::new(app::FingerGapApp::new()))
        }),
    )
}
