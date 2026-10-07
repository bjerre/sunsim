#include "button_controller.h"

#include "esp_log.h"

namespace sunsim {

namespace {
constexpr const char *kTag = "BUTTON";
}

ButtonController::ButtonController(Callbacks callbacks, std::chrono::milliseconds double_click_window)
    : callbacks_(std::move(callbacks)), double_click_window_(double_click_window)
{
}

void ButtonController::onOffOffCommand(bool on)
{
    const auto now = std::chrono::steady_clock::now();

    if (!on) {
        click_pending_ = false;
        enterManualOff();
        return;
    }

    if (click_pending_ && (now - last_click_) <= double_click_window_) {
        click_pending_ = false;
        enterRedAlert();
    } else {
        click_pending_ = true;
        last_click_    = now;
    }
}

void ButtonController::onSinglePress()
{
    click_pending_ = false;
    enterAuto();
}

void ButtonController::onDoublePress()
{
    click_pending_ = false;
    enterRedAlert();
}

void ButtonController::onHold()
{
    click_pending_ = false;
    enterManualOff();
}

void ButtonController::onSceneRecall()
{
    click_pending_ = false;
    enterRedAlert();
}

void ButtonController::tick()
{
    if (click_pending_ && (std::chrono::steady_clock::now() - last_click_) > double_click_window_) {
        click_pending_ = false;
        enterAuto();
    }

    if (mode_ == LightingMode::RedAlert && callbacks_.redAlertTick) {
        callbacks_.redAlertTick();
    }
}

void ButtonController::enterAuto()
{
    mode_ = LightingMode::Auto;
    ESP_LOGI(kTag, "Resuming daylight program");
    if (callbacks_.resumeAuto) {
        callbacks_.resumeAuto();
    }
}

void ButtonController::enterManualOff()
{
    mode_ = LightingMode::ManualOff;
    ESP_LOGI(kTag, "Manual off (press again to resume)");
    if (callbacks_.manualOff) {
        callbacks_.manualOff();
    }
}

void ButtonController::enterRedAlert()
{
    mode_ = LightingMode::RedAlert;
    ESP_LOGW(kTag, "RED ALERT");
    if (callbacks_.enterRedAlert) {
        callbacks_.enterRedAlert();
    }
}

} // namespace sunsim
