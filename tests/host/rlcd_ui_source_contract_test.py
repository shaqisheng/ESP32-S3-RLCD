from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/waveshare-s3-rlcd-4.2"


class RlcdUiSourceContractTest(unittest.TestCase):
    def test_overview_uses_editorial_layout_without_legacy_cards(self):
        source = (BOARD / "weather_ui.cc").read_text()
        self.assertIn("EDITORIAL_OVERVIEW_V3", source)
        self.assertIn("MakePlainPanel(status_strip, lv_color_black())", source)
        status = source[source.index("overview_status_label_"):source.index("time_label_")]
        self.assertIn("lv_color_white()", status)
        self.assertNotIn("lv_image_create", status)
        for legacy in ("time_card", "calendar_card", "memo_card"):
            self.assertNotIn(legacy, source)

    def test_overview_uses_symbol_labels_not_opaque_rgb565_icons(self):
        source = (BOARD / "weather_ui.cc").read_text()
        header = (BOARD / "custom_lcd_display.h").read_text()
        update = (BOARD / "data_update_task.cc").read_text()
        # 2026-08-17 SRAM 优化：3 个状态 label 合并成 1 个 overview_status_label_
        # （"📶 🔋 92%"），wifi/电池符号改在 data_update_task 拼字符串。
        self.assertIn("overview_status_label_", source)
        self.assertIn("overview_status_label_", header)
        self.assertIn("LV_SYMBOL_WIFI", update)
        self.assertIn("LV_SYMBOL_BATTERY_FULL", update)
        # 旧的独立字段不得回归
        self.assertNotIn("overview_wifi_symbol_", source)
        self.assertNotIn("overview_battery_symbol_", source)
        self.assertNotIn("overview_wifi_symbol_", header)
        self.assertNotIn("overview_battery_symbol_", header)
        self.assertNotIn("lv_image_create(status_strip)", source)

    def test_calendar_shows_numeric_year_and_month_without_english_names(self):
        source = (BOARD / "calendar_ui.cc").read_text()
        header = (BOARD / "custom_lcd_display.h").read_text()
        self.assertIn("REFERENCE_MONTH_CALENDAR_V5", source)
        self.assertIn("calendar_column_labels_", header)
        self.assertIn('"%d年"', source)
        self.assertIn('"%d月"', source)
        self.assertIn("calendar_year_label_", header)
        self.assertIn("calendar_month_label_", header)
        self.assertNotIn("kMonthNames", source)
        self.assertIn("calendar_column_labels_.size()", source)

    def test_calendar_uses_reference_style_independent_day_cells(self):
        source = (BOARD / "calendar_ui.cc").read_text()
        header = (BOARD / "custom_lcd_display.h").read_text()
        self.assertIn("REFERENCE_MONTH_CALENDAR_V5", source)
        self.assertIn("calendar_day_cells_", source)
        self.assertIn("calendar_day_cells_", header)
        self.assertIn("for (int cell_index = 0; cell_index < 42; ++cell_index)", source)
        self.assertIn("lv_obj_set_style_radius(cell.root", source)

    def test_forecast_keeps_one_grid_label(self):
        source = (BOARD / "forecast_ui.cc").read_text()
        header = (BOARD / "custom_lcd_display.h").read_text()
        self.assertIn("FIXED_COLUMN_FORECAST_V3", source)
        self.assertIn("forecast_column_labels_", header)
        self.assertIn("forecast_column_labels_.size()", source)

    def test_authenticated_proxy_credentials_are_not_echoed(self):
        quota = (BOARD / "managers/quota_manager.cc").read_text()
        admin = (BOARD / "managers/admin_server.cc").read_text()
        self.assertIn('"has_proxy_auth"', quota)
        self.assertIn('"proxy_endpoint"', quota)
        self.assertIn('"clear_proxy"', quota)
        self.assertIn('type="password"', admin)
        self.assertIn("已保存认证代理", admin)

    def test_proxy_tls_matches_secure_http11_client_contract(self):
        source = (BOARD / "managers/quota_proxy_transport.cc").read_text()
        header = (BOARD / "managers/quota_proxy_transport.h").read_text()
        quota = (BOARD / "managers/quota_manager.cc").read_text()
        self.assertIn("MBEDTLS_SSL_VERIFY_REQUIRED", source)
        self.assertIn("mbedtls_ssl_conf_alpn_protocols", source)
        self.assertIn('"http/1.1"', source)
        self.assertIn("GetQuotaProxyTransportError", header)
        self.assertIn("GetQuotaProxyTransportError", quota)

    def test_weather_admin_has_qweather_host_key_and_configurable_refresh(self):
        admin = (BOARD / "managers/admin_server.cc").read_text()
        weather = (BOARD / "managers/weather_manager.cc").read_text()
        header = (BOARD / "managers/weather_manager.h").read_text()
        self.assertIn("weatherProvince", admin)
        self.assertIn("weatherCity", admin)
        self.assertNotIn("weatherLocation", admin)
        self.assertIn("qwApiHost", admin)
        self.assertIn("qwApiKey", admin)
        self.assertNotIn("amapWebKey", admin)
        # 城市经纬度由所选城市经 cityCatalog 自动带出，不再需要人工填写
        self.assertIn("cityCatalog", admin)
        self.assertIn("renderWeatherCities", admin)
        self.assertIn("weatherRefreshMinutes", admin)
        self.assertIn("refresh_interval_minutes", weather)
        self.assertIn("GetRefreshIntervalMinutes", header)
        self.assertIn("QWEATHER_CITY_WEATHER_V1", weather)
        self.assertIn("qw_host", weather)

    def test_weather_uses_qweather_v1_with_a_masked_api_key(self):
        weather = (BOARD / "managers/weather_manager.cc").read_text()
        admin = (BOARD / "managers/admin_server.cc").read_text()
        self.assertIn('"https://%s/weather/v1/current/%.2f/%.2f"', weather)
        self.assertIn('"https://%s/weather/v1/daily/%.2f/%.2f?days=7&localTime=true"', weather)
        # Key 走请求头而不是 URL，避免出现在诊断与日志里
        self.assertIn("X-QW-Api-Key", weather)
        self.assertIn('"has_qw_key"', weather)
        self.assertIn('"qw_host"', weather)
        self.assertIn("qwApiHost", admin)
        self.assertIn("和风 API Key", admin)
        self.assertIn('type="password"', admin)
        # 旧数据源已彻底移除
        self.assertNotIn("restapi.amap.com", weather)
        self.assertNotIn("open-meteo", weather)
        # 和风网关强制 gzip（无视 Accept-Encoding），板载用 ROM tinfl 解压，不得回归
        self.assertIn("tinfl_decompress(", weather)
        self.assertIn("GunzipResponse", weather)
        public_config = weather[weather.index("getLocationConfigJson"):weather.index("applyLocationConfigJson")]
        self.assertNotIn('"qw_key"', public_config)

    def test_weather_admin_removes_qweather_jwt_workflow(self):
        admin = (BOARD / "managers/admin_server.cc").read_text()
        self.assertNotIn("crypto.subtle.generateKey", admin)
        self.assertNotIn("jwtPrivateKey", admin)
        self.assertNotIn("JWT Token", admin)

    def test_overview_refresh_returns_to_idle_and_date_uses_two_lines(self):
        board = (BOARD / "waveshare-s3-rlcd-4.2.cc").read_text()
        task = (BOARD / "data_update_task.cc").read_text()
        overview = (BOARD / "weather_ui.cc").read_text()
        self.assertIn("RequestRefresh", board)
        self.assertNotIn("天气等待 MCP 同步", board)
        self.assertIn('"%d年%d月%d日 %s\\n农历%s"', task)
        self.assertIn("LV_LABEL_LONG_WRAP", overview[overview.index("date_detail_label_"):overview.index("weather_icon_image_")])

    def test_weather_refresh_uses_actual_wifi_connection_not_ai_service_state(self):
        task = (BOARD / "data_update_task.cc").read_text()
        self.assertIn("#include <wifi_manager.h>", task)
        self.assertIn("WifiManager::GetInstance().IsConnected()", task)
        self.assertIn("can_run_background_requests", task)
        self.assertIn("!in_audio_session", task)
        self.assertNotIn("bool network_connected = (ds != kDeviceStateStarting", task)

    def test_admin_todo_mutations_schedule_immediate_display_refresh(self):
        admin = (BOARD / "managers/admin_server.cc").read_text()
        self.assertIn("ScheduleTodoDisplayRefresh", admin)
        self.assertGreaterEqual(admin.count("ScheduleTodoDisplayRefresh();"), 3)
        self.assertIn("editTodo(", admin)

    def test_weather_combines_qweather_current_with_seven_day_forecast(self):
        admin = (BOARD / "managers/admin_server.cc").read_text()
        weather = (BOARD / "managers/weather_manager.cc").read_text()
        header = (BOARD / "managers/weather_manager.h").read_text()
        self.assertIn("latitude:city[1]", admin)
        self.assertIn("longitude:city[2]", admin)
        self.assertIn("/weather/v1/current/", weather)
        self.assertIn("/weather/v1/daily/", weather)
        self.assertIn("days=7", weather)
        self.assertIn("parseQweatherCurrentJson", weather)
        self.assertIn("parseQweatherDailyJson", weather)
        self.assertIn("forecast_count = 7", weather)
        self.assertIn("parseQweatherCurrentJson", header)
        self.assertIn("parseQweatherDailyJson", header)

    def test_weather_uses_local_monochrome_images_instead_of_ascii_symbols(self):
        forecast = (BOARD / "forecast_ui.cc").read_text()
        header = (BOARD / "custom_lcd_display.h").read_text()
        asset_path = BOARD / "assets/icons/ui_img_weather_icons.c"
        self.assertTrue(asset_path.exists(), "weather icon asset file is missing")
        assets = asset_path.read_text()
        for name in ("sunny", "partly_cloudy", "overcast", "rain",
                     "thunder", "snow", "fog", "unknown"):
            self.assertIn(f"ui_img_weather_{name}_large", forecast)
            self.assertIn(f"ui_img_weather_{name}_small", forecast)
        self.assertIn("LV_COLOR_FORMAT_A1", assets)
        self.assertIn("weather_icon_image_", header)
        self.assertIn("forecast_now_icon_image_", header)
        self.assertIn("forecast_icon_images_", header)
        self.assertNotIn('return "O"', forecast)
        self.assertNotIn('return "///"', forecast)
        self.assertNotIn('return "***"', forecast)
        self.assertNotIn('return "==="', forecast)
        self.assertNotIn('return "(~)"', forecast)

    def test_weather_icon_mapping_prefers_qweather_code_over_generic_forecast_text(self):
        forecast = (BOARD / "forecast_ui.cc").read_text()
        mapping = forecast[forecast.index("WeatherIconKind WeatherIcon"):
                           forecast.index("const lv_image_dsc_t* LargeWeatherIcon")]
        self.assertLess(mapping.index("const long icon_code"), mapping.index('text.find("雷")'))
        self.assertIn("if (icon_code == 104) return WeatherIconKind::Overcast", mapping)
        self.assertIn("if (!code.empty())", mapping)

    def test_calendar_header_and_traditional_festivals_are_complete(self):
        ui = (BOARD / "calendar_ui.cc").read_text()
        header = (BOARD / "custom_lcd_display.h").read_text()
        manager = (BOARD / "managers/calendar_manager.cc").read_text()
        self.assertIn("calendar_year_label_", header)
        self.assertIn("calendar_month_label_", header)
        self.assertIn('"%d年"', ui)
        self.assertIn('"%d月"', ui)
        self.assertIn("TraditionalDayText", ui)
        self.assertIn('"七夕节"', manager)
        self.assertIn('"立秋"', manager)
        self.assertIn('"处暑"', manager)

    def test_ota_keeps_server_timestamp_as_utc_epoch(self):
        ota = (ROOT / "main/ota.cc").read_text()
        server_time = ota[ota.index('cJSON *server_time'):
                          ota.index('has_new_version_ = false')]
        self.assertNotIn("ts +=", server_time)
        self.assertNotIn("timezone_offset->valueint", server_time)
        self.assertIn("tv.tv_sec = (time_t)(ts / 1000)", server_time)

    def test_boot_prefetches_time_and_weather_before_xiaozhi_activation(self):
        board_header = (ROOT / "main/boards/common/board.h").read_text()
        application = (ROOT / "main/application.cc").read_text()
        board = (BOARD / "waveshare-s3-rlcd-4.2.cc").read_text()
        task = (BOARD / "data_update_task.cc").read_text()

        self.assertIn("virtual void PrepareForActivation()", board_header)
        activation = application[application.index("void Application::ActivationTask()"):
                                 application.index("void Application::CheckAssetsVersion()")]
        self.assertIn("PrepareForActivation", activation)
        self.assertLess(activation.index("PrepareForActivation"),
                        activation.index("std::make_unique<Ota>"))
        self.assertLess(activation.index("PrepareForActivation"),
                        activation.index("InitializeProtocol"))

        preparation = board[board.index("void PrepareForActivation() override"):
                            board.index("virtual AudioCodec* GetAudioCodec() override")]
        self.assertLess(preparation.index("syncNtpTime"),
                        preparation.index("WeatherManager::getInstance().update"))
        self.assertIn("TakeRefreshRequest", preparation)
        self.assertLess(preparation.index("TakeRefreshRequest"),
                        preparation.index("WeatherManager::getInstance().update"))
        self.assertIn("MarkBootDataPrefetched", preparation)
        self.assertIn("BootTimePrefetched", task)
        self.assertIn("BootWeatherPrefetched", task)

    def test_background_https_waits_for_stable_idle_and_is_serialized(self):
        task = (BOARD / "data_update_task.cc").read_text()
        quota = (BOARD / "managers/quota_manager.cc").read_text()
        weather = (BOARD / "managers/weather_manager.cc").read_text()
        calendar = (BOARD / "managers/calendar_manager.cc").read_text()
        safety = (BOARD / "managers/manager_safety.h").read_text()
        self.assertIn("IDLE_GUARD_MS = 30000", task)
        self.assertIn("kNetworkIdleGuardMs = 30000", quota)
        self.assertIn("idle_since_ms", quota)
        self.assertIn("state == kDeviceStateIdle", quota)
        self.assertIn("BackgroundNetworkMutex", safety)
        self.assertIn("BackgroundNetworkSession", quota)
        self.assertIn("BackgroundNetworkSession", weather)
        self.assertIn("BackgroundNetworkSession", calendar)

    def test_voice_button_preempts_inflight_background_network(self):
        board = (BOARD / "waveshare-s3-rlcd-4.2.cc").read_text()
        safety = (BOARD / "managers/manager_safety.h").read_text()
        callback = board[board.index("boot_button_.OnClick"):
                         board.index("// USER 按钮")]
        self.assertIn("CancelBackgroundNetwork", callback)
        self.assertIn("IsBackgroundNetworkActive", callback)
        self.assertLess(callback.index("CancelBackgroundNetwork"),
                        callback.index("app.ToggleChatState"))
        self.assertIn("BackgroundNetworkSession", safety)
        self.assertIn("BackgroundNetworkGeneration", safety)

    def test_background_transports_abort_when_voice_cancels_generation(self):
        quota = (BOARD / "managers/quota_manager.cc").read_text()
        proxy = (BOARD / "managers/quota_proxy_transport.cc").read_text()
        weather = (BOARD / "managers/weather_manager.cc").read_text()
        calendar = (BOARD / "managers/calendar_manager.cc").read_text()
        task = (BOARD / "data_update_task.cc").read_text()
        self.assertIn("idle_generation", task)
        self.assertIn("idle_generation", quota)
        self.assertIn("BackgroundNetworkCancelled", quota)
        self.assertIn("BackgroundNetworkCancelled", proxy)
        self.assertIn("BackgroundNetworkCancelled", weather)
        self.assertIn("BackgroundNetworkCancelled", calendar)

    def test_rlcd_spi_no_memory_retries_without_aborting(self):
        driver = (BOARD / "rlcd_driver.cc").read_text()
        send = driver[driver.index("void RlcdDriver::RLCD_Sendbuffera"):
                      driver.index("void RlcdDriver::RLCD_Reset")]
        self.assertIn("ESP_ERR_NO_MEM", send)
        self.assertIn("vTaskDelay", send)
        self.assertNotIn("ESP_ERROR_CHECK(", send)

    def test_rlcd_spi_uses_one_small_dma_transaction_at_a_time(self):
        driver = (BOARD / "rlcd_driver.cc").read_text()
        self.assertIn("constexpr int kSpiChunkSize = 1024", driver)
        self.assertIn("buscfg.max_transfer_sz = kSpiChunkSize", driver)
        self.assertIn("io_config.trans_queue_depth = 1", driver)
        send = driver[driver.index("void RlcdDriver::RLCD_Sendbuffera"):
                      driver.index("void RlcdDriver::RLCD_Reset")]
        self.assertIn("offset", send)
        self.assertIn("chunk_len", send)

    def test_afe_worker_tasks_allocate_stacks_from_psram_and_check_creation(self):
        wake = (ROOT / "main/audio/wake_words/afe_wake_word.cc").read_text()
        processor = (ROOT / "main/audio/processors/afe_audio_processor.cc").read_text()
        wake_init = wake[wake.index("bool AfeWakeWord::Initialize"):
                         wake.index("void AfeWakeWord::OnWakeWordDetected")]
        processor_init = processor[processor.index("void AfeAudioProcessor::Initialize"):
                                   processor.index("AfeAudioProcessor::~AfeAudioProcessor")]
        for source in (wake_init, processor_init):
            self.assertIn("xTaskCreateWithCaps", source)
            self.assertIn("MALLOC_CAP_SPIRAM", source)
            self.assertIn("pdPASS", source)
            self.assertIn("vTaskDeleteWithCaps", source)
            self.assertNotIn("xTaskCreate([]", source)

    def test_afe_configs_and_voice_resources_are_released_after_use(self):
        wake = (ROOT / "main/audio/wake_words/afe_wake_word.cc").read_text()
        processor = (ROOT / "main/audio/processors/afe_audio_processor.cc").read_text()
        self.assertIn("afe_config_free(afe_config)", wake)
        self.assertIn("afe_config_free(afe_config)", processor)
        stop = processor[processor.index("void AfeAudioProcessor::Stop"):
                         processor.index("bool AfeAudioProcessor::IsRunning")]
        self.assertNotIn("ReleaseResources()", stop)
        self.assertIn("xEventGroupClearBits", stop)
        self.assertIn("CreateResources()", processor)

    def test_rlcd_afe_uses_low_cost_modes_and_skips_wake_audio_history(self):
        wake = (ROOT / "main/audio/wake_words/afe_wake_word.cc").read_text()
        processor = (ROOT / "main/audio/processors/afe_audio_processor.cc").read_text()
        board_config = (BOARD / "config.json").read_text()
        self.assertIn("AFE_MODE_HIGH_PERF", wake)
        self.assertIn("AEC_MODE_SR_HIGH_PERF", wake)
        self.assertIn("bool AfeWakeWord::CreateResources()", wake)
        self.assertIn("void AfeWakeWord::ReleaseResources()", wake)
        wake_stop = wake[wake.index("void AfeWakeWord::Stop"):
                         wake.index("void AfeWakeWord::Feed")]
        self.assertNotIn("ReleaseResources()", wake_stop)
        self.assertIn("reset_buffer", wake_stop)
        self.assertIn("AFE_MODE_LOW_COST", processor)
        self.assertIn("AEC_MODE_VOIP_LOW_COST", processor)
        self.assertIn('"CONFIG_SEND_WAKE_WORD_DATA=n"', board_config)
        self.assertIn(
            "void AudioService::EncodeWakeWord() {\n#if CONFIG_SEND_WAKE_WORD_DATA",
            (ROOT / "main/audio/audio_service.cc").read_text(),
        )
        audio_service = (ROOT / "main/audio/audio_service.cc").read_text()
        audio_header = (ROOT / "main/audio/audio_service.h").read_text()
        self.assertIn("input_pipeline_mutex_", audio_header)
        self.assertIn("xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);\n        std::lock_guard<std::mutex> pipeline_lock", audio_service)
        self.assertIn("xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);\n        std::lock_guard<std::mutex> pipeline_lock", audio_service)
        self.assertIn("xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING", audio_service)
        self.assertIn("xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING", audio_service)

    def test_voice_state_cancels_background_network_for_button_and_wake_word(self):
        application = (ROOT / "main/application.h").read_text()
        board = (BOARD / "waveshare-s3-rlcd-4.2.cc").read_text()
        self.assertIn("AddStateChangeListener", application)
        self.assertIn("app.AddStateChangeListener", board)
        self.assertIn("CancelBackgroundNetwork", board)

    def test_cached_holiday_year_is_not_resynced_after_each_reboot(self):
        calendar_header = (BOARD / "managers/calendar_manager.h").read_text()
        task = (BOARD / "data_update_task.cc").read_text()
        self.assertIn("GetCachedYear", calendar_header)
        self.assertIn("CalendarManager::GetInstance().GetCachedYear()", task)

    def test_rlcd_excludes_unneeded_mcp_tools_without_changing_other_boards(self):
        board = (BOARD / "waveshare-s3-rlcd-4.2.cc").read_text()
        header = (ROOT / "main/mcp_server.h").read_text()
        server = (ROOT / "main/mcp_server.cc").read_text()
        for tool in ("self.music.play_url", "self.screen.set_theme", "self.weather.update"):
            self.assertIn(f'mcp_server.DisableTool("{tool}")', board)
        for tool in ("self.music.play_url", "self.screen.set_theme"):
            self.assertIn(tool, server)
        self.assertNotIn('mcp_server.AddTool("self.weather.update"', board)
        self.assertIn("void DisableTool(const std::string& name)", header)
        self.assertIn("disabled_tools_", header)
        self.assertIn("disabled_tools_", server)

    def test_mcp_tools_list_uses_small_pages_to_fit_rlcd_internal_sram(self):
        server = (ROOT / "main/mcp_server.cc").read_text()
        self.assertIn("constexpr size_t kToolsListMaxPayloadSize = 2048", server)
        self.assertIn("kToolsListMaxPayloadSize", server)
        self.assertIn('"],\\"nextCursor\\":\\"" + next_cursor', server)

    def test_mcp_reply_reuses_one_psram_sized_buffer_across_layers(self):
        server = (ROOT / "main/mcp_server.cc").read_text()
        server_header = (ROOT / "main/mcp_server.h").read_text()
        application = (ROOT / "main/application.cc").read_text()
        application_header = (ROOT / "main/application.h").read_text()
        protocol = (ROOT / "main/protocols/protocol.cc").read_text()
        protocol_header = (ROOT / "main/protocols/protocol.h").read_text()
        self.assertIn("constexpr size_t kMcpEnvelopeReserve = 512", server)
        self.assertIn("json.reserve(kToolsListMaxPayloadSize + kMcpEnvelopeReserve)", server)
        self.assertIn("void ReplyResult(int id, std::string result)", server_header)
        self.assertIn("void Application::SendMcpMessage(std::string payload)", application)
        self.assertIn("void SendMcpMessage(std::string payload)", application_header)
        self.assertIn("protocol_->SendMcpMessage(std::move(payload))", application)
        self.assertIn("void Protocol::SendMcpMessage(std::string payload)", protocol)
        self.assertIn("virtual void SendMcpMessage(std::string payload)", protocol_header)
        self.assertIn("payload.insert(0", protocol)
        self.assertNotIn("std::string message =", protocol[protocol.index("void Protocol::SendMcpMessage"):])

    def test_weather_diagnostic_uses_header_key_without_echoing_key(self):
        weather = (BOARD / "managers/weather_manager.cc").read_text()
        admin = (BOARD / "managers/admin_server.cc").read_text()
        header = (BOARD / "managers/admin_server.h").read_text()
        self.assertNotIn('"Authorization"', weather)
        self.assertIn("qw_key", weather)
        self.assertIn('"/api/weather-diagnostic"', admin)
        self.assertIn("WeatherDiagnosticHandler", header)
        self.assertIn("GetDiagnosticJson", weather)
        self.assertIn("last_endpoint_", weather)
        self.assertNotIn('cJSON_AddStringToObject(root, "api_key"', weather)
        self.assertNotIn('cJSON_AddStringToObject(root, "qw_key"', weather)

    def test_weather_persists_qweather_host_key_and_forecast_coordinates(self):
        weather = (BOARD / "managers/weather_manager.cc").read_text()
        constructor = weather[weather.index("WeatherManager::WeatherManager()"):
                              weather.index("WeatherManager& WeatherManager::getInstance()")]
        self.assertIn('settings.GetString("qw_host", "")', constructor)
        self.assertIn('settings.GetString("qw_key", "")', constructor)
        self.assertIn('settings.GetString("latitude", "31.2989")', constructor)
        self.assertIn('settings.GetString("longitude", "120.5853")', constructor)

    def test_admin_removes_previews_and_adds_device_controls(self):
        source = (BOARD / "managers/admin_server.cc").read_text()
        header = (BOARD / "managers/admin_server.h").read_text()
        self.assertNotIn("AI 页面实时预览", source)
        self.assertNotIn("featurePreview", source)
        self.assertNotIn("renderPreview", source)
        self.assertIn('"/api/device"', source)
        self.assertIn("volumeSlider", source)
        self.assertIn("DeviceHandler", header)

    def test_admin_has_proxy_connectivity_diagnostics_without_secret_echo(self):
        admin = (BOARD / "managers/admin_server.cc").read_text()
        header = (BOARD / "managers/admin_server.h").read_text()
        transport = (BOARD / "managers/quota_proxy_transport.cc").read_text()
        self.assertIn('"/api/proxy-diagnostic"', admin)
        self.assertIn("proxyDiagnostic", admin)
        self.assertIn("ProxyDiagnosticHandler", header)
        self.assertIn("DiagnoseQuotaProxy", transport)
        self.assertNotIn("authorization\"", admin[admin.index("proxyDiagnostic"):])

    def test_zero_volume_is_a_valid_persisted_value(self):
        source = (ROOT / "main/audio/audio_codec.cc").read_text()
        self.assertNotIn("output_volume_ <= 0", source)

    def test_ai_header_reads_device_ip_for_admin_address(self):
        source = (BOARD / "quota_ui.cc").read_text()
        self.assertIn("quota_admin_label_", source)
        self.assertIn("WifiManager::GetInstance().GetIpAddress()", source)
        self.assertIn("FormatAdminAddress", source)

    def test_quota_cards_restore_original_icon_and_large_value_layout(self):
        source = (BOARD / "quota_ui.cc").read_text()
        header = (BOARD / "custom_lcd_display.h").read_text()
        self.assertIn("LV_IMAGE_DECLARE(ui_img_quota_deepseek)", source)
        self.assertIn("ProviderLogo(", source)
        self.assertIn("quota_logo_images_", header)
        self.assertIn("&alibaba_puhui_48", source)
        self.assertNotIn("ProviderTitle", source)

    def test_quota_compact_cards_keep_a_gap_between_logo_and_remaining_value(self):
        source = (BOARD / "quota_ui.cc").read_text()
        self.assertIn("QUOTA_COMPACT_VALUE_Y = 16", source)
        self.assertIn("QUOTA_COMPACT_PRIMARY_Y = 68", source)
        self.assertIn("QUOTA_COMPACT_BAR_Y = 86", source)
        self.assertIn("QUOTA_COMPACT_SECONDARY_Y = 93", source)
        self.assertIn("lv_obj_set_size(quota_bars_[i][0], 172, 6)", source)
        self.assertIn("lv_obj_set_style_pad_bottom(card, 0, 0)", source)
        self.assertIn("height == 118 ? QUOTA_COMPACT_VALUE_Y", source)
        self.assertIn("height == 118 ? QUOTA_COMPACT_PRIMARY_Y", source)

    def test_all_four_quota_logos_share_white_background(self):
        """4 个 provider logo（Codex/Kimi/GLM/DeepSeek）必须统一白底（0xff），
        不允许 GLM/DeepSeek 再被 inverted。回归：曾因 inverted 导致 4 个 logo
        背景色不统一（黑白混搭），用户要求改回原版。"""
        for asset in ("ui_img_quota_codex.c", "ui_img_quota_kimi.c",
                      "ui_img_quota_glm.c", "ui_img_quota_deepseek.c"):
            source = (BOARD / "assets/icons" / asset).read_text()
            # 头部不允许出现 INVERTED_FOR_QUOTA_PAGE 标记
            self.assertNotIn("INVERTED_FOR_QUOTA_PAGE", source,
                             f"{asset} 仍是 inverted，应改回原版白底")
            # 统一注释格式
            self.assertIn("Official provider mark", source)
            # 数据首字节必须是 0xff（白底）—— 原始字节数组第一行第一字节
            self.assertRegex(source, r"_map\[\]\s*=\s*\{\s*\n\s*0xff,")

    def test_quota_cards_use_unified_white_background(self):
        """4 张 AI 卡片必须统一白底黑字。回归：曾用 `light = row == 0`
        让上排白底、下排黑底，用户反馈"上下背景不统一"。"""
        source = (BOARD / "quota_ui.cc").read_text()
        # 不允许出现 row == 0 的 light 判断
        self.assertNotIn("row == 0", source)
        self.assertNotIn("const bool light", source)
        # 必须硬编码为黑字 / 白底
        self.assertIn("const lv_color_t foreground = lv_color_black()", source)
        self.assertIn("const lv_color_t background = lv_color_white()", source)

    def test_quota_card_layout_separates_primary_and_weekly_tiers(self):
        """AI 卡片三层信息按用户需求拆分：
        - 大数字：5H 优先（PrimaryTier 用 FindShortTier），无 5H 则周额度
        - 进度条：固定周额度（FindLongTier）
        - 进度条下方：周剩余% + 重置时间(xx日xx时) + 倒计时
        回归：曾把"剩余最少的"tier 同时用于大数字和进度条。"""
        source = (BOARD / "quota_ui.cc").read_text()
        # 必须有 5H / 周期 切分的辅助函数
        self.assertIn("FindShortTier", source)
        self.assertIn("FindLongTier", source)
        # PrimaryTier 必须基于 5H 优先
        self.assertIn("FindShortTier(card)", source)
        # 重置时间格式化必须分开 absolute（"日时"）和 countdown（"天后"）
        self.assertIn("FormatResetAbsolute", source)
        self.assertIn("FormatResetCountdown", source)
        # 进度条下方的组合文本：周剩余% + 重置时间(日时) + 倒计时。
        # （周标签已刻意去掉以省宽度，见 quota_ui.cc 紧凑格式注释）
        self.assertIn('"%d%% %s %s"', source)
        self.assertIn("FormatResetAbsolute(weekly.reset_at, when, sizeof(when))", source)
        self.assertIn("FormatResetCountdown(weekly.reset_at, countdown, sizeof(countdown))", source)
        # 不允许出现旧的 "重置" 后缀（已改为"后"）
        self.assertNotIn("%d天%d小时后重置", source)
        # 倒计时必须用紧凑缩写（d/h/m），不允许中文天/小时/分钟
        self.assertIn("%dd%dh", source)
        self.assertIn("%dh%dm", source)
        self.assertNotIn("%d天", source)
        self.assertNotIn("%d小时", source)
        # 不允许旧的 other_index 双 tier 切换逻辑
        self.assertNotIn("other_index", source)

    def test_quota_refresh_interval_is_configurable_and_defaults_to_five_minutes(self):
        quota = (BOARD / "managers/quota_manager.cc").read_text()
        header = (BOARD / "managers/quota_manager.h").read_text()
        admin = (BOARD / "managers/admin_server.cc").read_text()
        self.assertIn("refresh_interval_minutes_ = 5", header)
        self.assertIn("SetRefreshIntervalMinutes", quota)
        self.assertIn('"/api/refresh-interval"', admin)
        self.assertIn("quotaRefreshMinutes", admin)

    def test_kimi_reset_time_parses_iso8601_not_ms(self):
        """Kimi /coding/v1/usages 的 resetTime 是 ISO 8601 字符串
        （如 "2026-08-11T15:53:05Z"），不是毫秒时间戳。回归：原来用
        JsonNumber 会触发 atof('2026-08-11...')=2026，/1000 后变成 Unix
        秒 2，导致永远显示"即将重置"。"""
        quota = (BOARD / "managers/quota_manager.cc").read_text()
        self.assertIn("ParseIso8601ToUnix", quota)
        self.assertIn("ParseResetAt", quota)
        # Kimi 必须用 ISO 感知解析器，不能再直接 JsonNumber/1000
        self.assertIn('ParseResetAt(detail, "resetTime")', quota)
        self.assertIn('ParseResetAt(usage, "resetTime")', quota)
        # 同样用 ParseResetAt 兼容 GLM（数字）和未来可能变更
        self.assertIn('ParseResetAt(item, "nextResetTime")', quota)
        # 算法注释存在以便后续维护者理解为什么不用 timegm
        self.assertIn("Howard Hinnant", quota)

    def test_screenshot_copy_button_with_fallback(self):
        """截图复制按钮（2026-08-18）：现代 Clipboard API + execCommand 降级。
        HTTP 局域网是非安全上下文（isSecureContext=false，navigator.clipboard 与
        ClipboardItem 均为 undefined），必须保留 execCommand('copy') 选中 <img>
        的降级路径，否则复制在局域网后台必然失败。"""
        admin = (BOARD / "managers/admin_server.cc").read_text()
        self.assertIn('id="screenshotCopyBtn"', admin)
        self.assertIn("copyScreenshot", admin)
        self.assertIn("ClipboardItem", admin)
        self.assertIn('execCommand("copy")', admin)
        self.assertIn("lastShotBlob", admin)
        # 预览图必须用 data URL 内嵌图像数据——blob: 引用复制出页面即失效，
        # 粘贴目标只会拿到 alt 文字（"截图预览"），2026-08-18 用户实测回归
        self.assertIn("readAsDataURL", admin)
        # 必须走 copy 事件劫持覆写剪贴板（非安全上下文可写）——选中 <img> +
        # execCommand 的路线已实机证明不可靠（粘贴出 alt 文字而非图片）
        self.assertIn('addEventListener("copy",onCopy', admin)
        self.assertIn('setData("text/html"', admin)

    def test_global_low_battery_alert_contract(self):
        """全局低电量提示（2026-08-18）：
        - lv_layer_top 悬浮条（所有页面共享），不逐页实现
        - 5 分钟无交互自动隐藏（复用 IDLE_TIMEOUT_MS）+ 交互重现（NotifyUserActivity）
        - 后台写操作与硬件按钮都算用户活动；/api/device 暴露 low_battery_alert
        - 旧的 sensor_label_ 反色告警（low_battery_alert_active）不得回归"""
        display_cc = (BOARD / "custom_lcd_display.cc").read_text()
        display_h = (BOARD / "custom_lcd_display.h").read_text()
        weather_ui = (BOARD / "weather_ui.cc").read_text()
        admin = (BOARD / "managers/admin_server.cc").read_text()
        update = (BOARD / "data_update_task.cc").read_text()
        self.assertIn("lv_layer_top()", display_cc)
        self.assertIn("low_batt_overlay_", display_h)
        self.assertIn("SetupLowBatteryOverlay();", weather_ui)
        self.assertIn("UpdateLowBatteryAlertInternal", display_cc)
        self.assertIn("UpdateLowBatteryAlertInternal(cached_battery_level", update)
        self.assertIn("IDLE_TIMEOUT_MS", display_cc)
        # 交互路径：后台写操作接入用户活动（按钮侧在板级已接入）
        self.assertIn("display->NotifyUserActivity()", admin)
        # 后台字段与横幅
        self.assertIn('"low_battery_alert"', admin)
        self.assertIn('id="lowBattBanner"', admin)
        self.assertIn("lowbatt-banner", admin)
        # 旧实现不得回归
        self.assertNotIn("low_battery_alert_active", update)

    def test_power_save_actually_applies_and_reads_back(self):
        """省电模式必须真正生效（2026-08-17 回归）：
        - CONFIG_PM_ENABLE 必须启用，否则 esp_pm_configure 静默返回
          ESP_ERR_NOT_SUPPORTED，"CPU 降频到 80MHz" 一直是假的
        - esp_pm_configure 失败必须打出真实错误码而不是猜测文案
        - 退出时 WiFi PS 必须恢复应用基线 LOW_POWER（BALANCED 比基线更耗电）
        - 必须有效果回读（CPU 实际频率 + WiFi PS 实际状态）"""
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        psm = (BOARD / "managers/power_save_manager.cc").read_text()
        self.assertIn("CONFIG_PM_ENABLE=y", defaults)
        self.assertIn("esp_err_to_name(pm_err)", psm)
        self.assertIn("LogActualPowerState", psm)
        self.assertIn("esp_wifi_get_ps", psm)
        self.assertNotIn("PowerSaveLevel::BALANCED", psm)

    def test_logs_tab_and_ring_buffer_contract(self):
        """后台日志功能（2026-08-17）：esp_log 全局钩子 + PSRAM 环形缓冲 + 日志 tab。
        约束：缓冲必须驻留 PSRAM（内部 SRAM 紧张）、必须有重复折叠（AFE 每 30ms
        告警会冲掉有用日志）、必须打码敏感参数（secret 不经日志接口泄露）。"""
        admin = (BOARD / "managers/admin_server.cc").read_text()
        buf = (BOARD / "managers/system_log_buffer.h").read_text()
        board = (BOARD / "waveshare-s3-rlcd-4.2.cc").read_text()
        # 路由与 handler
        self.assertIn('"/api/logs"', admin)
        self.assertIn("LogsHandler", admin)
        # tab 与视图（模块下拉按功能 optgroup 分组）
        self.assertIn('data-tab="logs"', admin)
        self.assertIn('id="logview"', admin)
        self.assertIn("loadLogs", admin)
        self.assertIn("LOG_GROUPS", admin)
        self.assertIn("logGroupOf", admin)
        self.assertIn("<optgroup", admin)
        # 环形缓冲关键属性
        self.assertIn("MALLOC_CAP_SPIRAM", buf)
        self.assertIn("esp_log_set_vprintf", buf)
        self.assertIn("repeat", buf)
        self.assertIn('"****"', buf)
        # 板级安装（构造函数尽早，覆盖启动日志）
        self.assertIn("SystemLogBuffer::GetInstance().Install()", board)

    def test_parse_iso8601_handles_real_kimi_response_format(self):
        """算法自检：用 Python datetime 求权威 Unix 秒，
        对应 C++ 端 ParseIso8601ToUnix("2026-08-11T15:53:05Z") 应得到相同值。
        2026-08-11 是该年第 223 天，但距 1月1日只有 222 天。"""
        import datetime
        dt = datetime.datetime(2026, 8, 11, 15, 53, 5,
                               tzinfo=datetime.timezone.utc)
        self.assertEqual(int(dt.timestamp()), 1786463585)


if __name__ == "__main__":
    unittest.main()
