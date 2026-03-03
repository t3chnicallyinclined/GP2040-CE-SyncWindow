use eframe::egui;
use egui::{Color32, RichText, ScrollArea, Ui};
use egui_plot::{Bar, BarChart, Plot};

use crate::input::{format_button, GamepadInput};
use crate::stats::GapStats;

const TEAL: Color32 = Color32::from_rgb(0, 180, 216);
const GREEN: Color32 = Color32::from_rgb(80, 200, 80);
const YELLOW: Color32 = Color32::from_rgb(220, 180, 40);
const LOG_MAX: usize = 500;

struct LogEntry {
    attempt: usize,
    button_a: String,
    button_b: String,
    gap_ms: f64,
    running_avg: f64,
}

pub struct FingerGapApp {
    input: Option<GamepadInput>,
    stats: GapStats,
    log: Vec<LogEntry>,
    error_msg: Option<String>,
}

impl FingerGapApp {
    pub fn new() -> Self {
        let (input, error_msg) = match GamepadInput::new() {
            Ok(gi) => (Some(gi), None),
            Err(e) => (None, Some(format!("Gamepad init failed: {e}"))),
        };
        Self {
            input,
            stats: GapStats::new(),
            log: Vec::new(),
            error_msg,
        }
    }
}

impl eframe::App for FingerGapApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // Poll gamepad
        if let Some(ref mut input) = self.input {
            if let Some(pair) = input.poll() {
                self.stats.record(pair.gap_ms);
                let avg = self.stats.average();
                self.log.push(LogEntry {
                    attempt: self.stats.count(),
                    button_a: format_button(pair.button_a),
                    button_b: format_button(pair.button_b),
                    gap_ms: pair.gap_ms,
                    running_avg: avg,
                });
                if self.log.len() > LOG_MAX {
                    self.log.remove(0);
                }
            }
        }

        // Request fast repaint for responsive input polling
        ctx.request_repaint_after(std::time::Duration::from_millis(1));

        // === TOP BAR ===
        egui::TopBottomPanel::top("header").show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.heading(RichText::new("NOBD FINGER GAP TESTER").strong());
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if ui.button("Reset").clicked() {
                        self.stats.clear();
                        self.log.clear();
                    }
                });
            });

            ui.separator();

            // Controller status
            if let Some(ref err) = self.error_msg {
                ui.colored_label(Color32::RED, format!("Error: {err}"));
            } else if let Some(ref input) = self.input {
                if let Some(name) = input.connected_gamepad_name() {
                    ui.horizontal(|ui| {
                        ui.colored_label(GREEN, "\u{25CF}");
                        ui.label(format!("Controller: {name}"));
                    });
                } else {
                    ui.horizontal(|ui| {
                        ui.colored_label(YELLOW, "\u{25CF}");
                        ui.label("No controller detected. Connect a gamepad.");
                    });
                }
            }
        });

        // === BOTTOM: ATTEMPT LOG ===
        egui::TopBottomPanel::bottom("log_panel")
            .min_height(120.0)
            .resizable(true)
            .show(ctx, |ui| {
                ui.heading("Attempt Log");
                ui.separator();
                ScrollArea::vertical()
                    .stick_to_bottom(true)
                    .show(ui, |ui| {
                        for entry in self.log.iter().rev() {
                            ui.horizontal(|ui| {
                                ui.monospace(format!(
                                    "#{:>3}  {} + {}  gap: {:5.1}ms  (avg: {:.1}ms)",
                                    entry.attempt,
                                    entry.button_a,
                                    entry.button_b,
                                    entry.gap_ms,
                                    entry.running_avg,
                                ));
                            });
                        }
                    });
            });

        // === CENTER ===
        egui::CentralPanel::default().show(ctx, |ui| {
            // Recommended NOBD value
            if self.stats.count() > 0 {
                ui.add_space(8.0);
                egui::Frame::new()
                    .inner_margin(12.0)
                    .corner_radius(8.0)
                    .stroke(egui::Stroke::new(2.0, TEAL))
                    .show(ui, |ui| {
                        ui.vertical_centered(|ui| {
                            ui.label(
                                RichText::new("RECOMMENDED NOBD VALUE")
                                    .size(14.0)
                                    .color(Color32::GRAY),
                            );
                            ui.label(
                                RichText::new(format!("{} ms", self.stats.recommended_nobd()))
                                    .size(48.0)
                                    .strong()
                                    .color(TEAL),
                            );
                            ui.label(
                                RichText::new(format!(
                                    "covers your slowest gap of {:.1}ms + 2ms headroom",
                                    self.stats.max()
                                ))
                                .size(12.0)
                                .color(Color32::GRAY),
                            );
                        });
                    });
                ui.add_space(8.0);
            } else {
                ui.add_space(20.0);
                ui.vertical_centered(|ui| {
                    ui.label(
                        RichText::new("Press two buttons at the same time to start measuring")
                            .size(16.0)
                            .color(Color32::GRAY),
                    );
                    ui.add_space(4.0);
                    ui.label(
                        RichText::new("(like LP+HP for a dash)")
                            .size(13.0)
                            .color(Color32::DARK_GRAY),
                    );
                });
                ui.add_space(20.0);
            }

            ui.separator();

            // Stats + Histogram side by side
            let available = ui.available_size();
            ui.horizontal_top(|ui| {
                // Left: Live stats
                ui.vertical(|ui| {
                    ui.set_min_width(160.0);
                    ui.heading("Live Stats");
                    ui.add_space(8.0);
                    draw_stat(ui, "Attempts", &format!("{}", self.stats.count()));
                    if self.stats.count() > 0 {
                        draw_stat(ui, "Average", &format!("{:.1}ms", self.stats.average()));
                        draw_stat(ui, "Median", &format!("{:.1}ms", self.stats.median()));
                        draw_stat(ui, "Fastest", &format!("{:.1}ms", self.stats.min()));
                        draw_stat(ui, "Slowest", &format!("{:.1}ms", self.stats.max()));
                    }
                });

                ui.separator();

                // Right: Histogram
                ui.vertical(|ui| {
                    ui.heading("Distribution");
                    ui.add_space(4.0);

                    let buckets = self.stats.histogram_buckets();
                    if buckets.is_empty() {
                        ui.colored_label(Color32::DARK_GRAY, "No data yet");
                    } else {
                        let bars: Vec<Bar> = buckets
                            .iter()
                            .enumerate()
                            .filter(|(_, (_, count, _))| *count > 0)
                            .map(|(i, (_label, count, _pct))| {
                                Bar::new(i as f64, *count as f64)
                                    .width(0.7)
                                    .fill(TEAL)
                            })
                            .collect();

                        let labels: Vec<(usize, String)> = buckets
                            .iter()
                            .enumerate()
                            .map(|(i, (label, _, _))| (i, label.clone()))
                            .collect();

                        let chart_height = (available.y * 0.45).max(120.0).min(250.0);

                        Plot::new("gap_histogram")
                            .height(chart_height)
                            .allow_drag(false)
                            .allow_zoom(false)
                            .allow_scroll(false)
                            .allow_boxed_zoom(false)
                            .show_axes([true, true])
                            .x_axis_formatter(move |val, _range| {
                                let idx = val.value.round() as usize;
                                labels
                                    .iter()
                                    .find(|(i, _)| *i == idx)
                                    .map(|(_, l)| l.clone())
                                    .unwrap_or_default()
                            })
                            .y_axis_formatter(|val, _range| {
                                format!("{}", val.value as u32)
                            })
                            .show(ui, |plot_ui| {
                                plot_ui.bar_chart(BarChart::new("gaps".to_string(), bars));
                            });
                    }
                });
            });
        });
    }
}

fn draw_stat(ui: &mut Ui, label: &str, value: &str) {
    ui.horizontal(|ui| {
        ui.label(RichText::new(format!("{label}:")).color(Color32::GRAY));
        ui.label(RichText::new(value).strong());
    });
}
