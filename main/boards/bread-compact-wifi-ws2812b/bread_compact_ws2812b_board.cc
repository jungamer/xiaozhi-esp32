#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/circular_strip.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "BreadCompactWS2812B"

enum class LedMode {
    AUTO,
    RAINBOW,
    MUSIC_VISUALIZER,
    BREATHING,
    SCROLLING
};

class BreadCompactWS2812BBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button mode_button_;
    
    CircularStrip* led_strip_ = nullptr;
    LedMode current_led_mode_ = LedMode::AUTO;
    bool music_visualizer_active_ = false;
    std::vector<int16_t> audio_buffer_;
    std::mutex audio_mutex_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeLEDStrip() {
        led_strip_ = new CircularStrip(WS2812B_LED_STRIP_GPIO, WS2812B_LED_COUNT);
        led_strip_->SetBrightness(32, 8);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) volume = 100;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) volume = 0;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        mode_button_.OnClick([this]() {
            CycleLedMode();
        });

        mode_button_.OnLongPress([this]() {
            current_led_mode_ = LedMode::AUTO;
            GetDisplay()->ShowNotification("LED: Auto");
            led_strip_->OnStateChanged();
        });
    }

    void CycleLedMode() {
        switch (current_led_mode_) {
            case LedMode::AUTO:
                current_led_mode_ = LedMode::RAINBOW;
                RainbowEffect();
                GetDisplay()->ShowNotification("LED: Rainbow");
                break;
            case LedMode::RAINBOW:
                current_led_mode_ = LedMode::BREATHING;
                BreathingEffect();
                GetDisplay()->ShowNotification("LED: Breathing");
                break;
            case LedMode::BREATHING:
                current_led_mode_ = LedMode::SCROLLING;
                ScrollingEffect();
                GetDisplay()->ShowNotification("LED: Scrolling");
                break;
            case LedMode::SCROLLING:
                current_led_mode_ = LedMode::MUSIC_VISUALIZER;
                StartMusicVisualizer();
                GetDisplay()->ShowNotification("LED: Music");
                break;
            case LedMode::MUSIC_VISUALIZER:
                current_led_mode_ = LedMode::AUTO;
                StopMusicVisualizer();
                led_strip_->OnStateChanged();
                GetDisplay()->ShowNotification("LED: Auto");
                break;
        }
    }

    void RainbowEffect() {
        static uint8_t hue = 0;
        led_strip_->StartStripTask(50, [this, &hue]() {
            hue = (hue + 1) % 256;
            for (int i = 0; i < WS2812B_LED_COUNT; i++) {
                uint8_t led_hue = (hue + i * 32) % 256;
                auto rgb = HsvToRgb(led_hue, 255, 32);
                led_strip_->SetSingleColor(i, rgb);
            }
        });
    }

    void BreathingEffect() {
        StripColor low = {0, 0, 0};
        StripColor high = {32, 32, 32};
        led_strip_->Breathe(low, high, 50);
    }

    void ScrollingEffect() {
        StripColor low = {0, 0, 0};
        StripColor high = {32, 16, 0};
        led_strip_->Scroll(low, high, 2, 100);
    }

    void StartMusicVisualizer() {
        music_visualizer_active_ = true;
        xTaskCreate([](void* arg) {
            auto board = static_cast<BreadCompactWS2812BBoard*>(arg);
            board->MusicVisualizerTask();
            vTaskDelete(NULL);
        }, "music_visualizer", 4096, this, 2, NULL);
    }

    void StopMusicVisualizer() {
        music_visualizer_active_ = false;
    }

    void MusicVisualizerTask() {
        std::vector<int16_t> local_buffer;
        while (music_visualizer_active_) {
            {
                std::lock_guard<std::mutex> lock(audio_mutex_);
                local_buffer = audio_buffer_;
                audio_buffer_.clear();
            }

            if (!local_buffer.empty()) {
                UpdateMusicVisualizer(local_buffer);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void UpdateMusicVisualizer(const std::vector<int16_t>& data) {
        if (data.empty()) return;

        int samples_per_led = data.size() / WS2812B_LED_COUNT;
        std::vector<uint8_t> levels(WS2812B_LED_COUNT, 0);

        for (int i = 0; i < WS2812B_LED_COUNT; i++) {
            int start = i * samples_per_led;
            int end = start + samples_per_led;
            int64_t sum = 0;
            int count = 0;

            for (int j = start; j < end && j < data.size(); j++) {
                sum += abs(data[j]);
                count++;
            }

            if (count > 0) {
                uint8_t avg = static_cast<uint8_t>((sum / count) >> 8);
                levels[i] = std::min(avg, static_cast<uint8_t>(64));
            }
        }

        for (int i = 0; i < WS2812B_LED_COUNT; i++) {
            uint8_t level = levels[i];
            uint8_t r = level;
            uint8_t g = level > 32 ? level - 32 : 0;
            uint8_t b = 64 - level;
            led_strip_->SetSingleColor(i, {r, g, b});
        }
    }

    void FeedAudioData(const std::vector<int16_t>& data) {
        if (current_led_mode_ != LedMode::MUSIC_VISUALIZER) return;
        std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_buffer_.insert(audio_buffer_.end(), data.begin(), data.end());
        if (audio_buffer_.size() > 1024) {
            audio_buffer_.erase(audio_buffer_.begin(), audio_buffer_.begin() + (audio_buffer_.size() - 1024));
        }
    }

    StripColor HsvToRgb(uint8_t h, uint8_t s, uint8_t v) {
        if (s == 0) return {v, v, v};
        
        uint8_t region = h / 43;
        uint8_t remainder = (h - region * 43) * 6;
        uint8_t p = (v * (255 - s)) >> 8;
        uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
        uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

        switch (region) {
            case 0: return {v, t, p};
            case 1: return {q, v, p};
            case 2: return {p, v, t};
            case 3: return {p, q, v};
            case 4: return {t, p, v};
            default: return {v, p, q};
        }
    }

    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
    }

public:
    BreadCompactWS2812BBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        mode_button_(GPIO_NUM_38) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeLEDStrip();
        InitializeButtons();
        InitializeTools();
    }

    virtual ~BreadCompactWS2812BBoard() {
        if (led_strip_) delete led_strip_;
    }

    virtual Led* GetLed() override {
        return led_strip_;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, 
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
#endif
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    void OnAudioData(const std::vector<int16_t>& data) {
        FeedAudioData(data);
    }
};

DECLARE_BOARD(BreadCompactWS2812BBoard);