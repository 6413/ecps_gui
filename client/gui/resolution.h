// Part of ecps_gui_t, included inside the struct by gui.h: the aspect and resolution choices.

  struct resolution_gui_controls_t {
    int selected_aspect_ratio = 4;
    int selected_resolution = 0;

    uint32_t last_applied_width = 0;
    uint32_t last_applied_height = 0;

    void render_resolution_controls(bool with_aspect = true) {
      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      uint32_t current_width = 0, current_height = 0;
      if (rt) {
        current_width = rt->screen_encoder.config_.width;
        current_height = rt->screen_encoder.config_.height;
      }

      if (current_width != last_applied_width || current_height != last_applied_height) {
        sync_dropdown_to_current_resolution(current_width, current_height);
        last_applied_width = current_width;
        last_applied_height = current_height;
      }

      if (with_aspect) {
        render_aspect_row();
      }

      std::vector<resolution_system_t::resolution_t> available_resolutions = get_filtered_resolutions();

      std::vector<std::string> res_strings;
      std::vector<const char*> res_cstrings;

      bool found_current = false;
      for (int i = 0; i < available_resolutions.size(); i++) {
        const auto& res = available_resolutions[i];
        res_strings.push_back(res.name + " (" + res.category + ")");

        if (res.width == current_width && res.height == current_height) {
          selected_resolution = i;
          found_current = true;
        }
      }

      if (!found_current && current_width > 0 && current_height > 0) {
        resolution_system_t::resolution_t current_res;
        current_res.width = current_width;
        current_res.height = current_height;
        current_res.name = std::to_string(current_width) + "x" + std::to_string(current_height);
        current_res.category = "Current";

        available_resolutions.insert(available_resolutions.begin(), current_res);
        res_strings.insert(res_strings.begin(), current_res.name + " (" + current_res.category + ")");
        selected_resolution = 0;
      }

      for (const auto& str : res_strings) {
        res_cstrings.push_back(str.c_str());
      }

      gui::settings_row("Resolution", [&] {
        if (gui::combo("##resolution", &selected_resolution, res_cstrings.data(), res_cstrings.size())) {
          if (selected_resolution >= 0 && selected_resolution < available_resolutions.size()) {
            apply_resolution(available_resolutions[selected_resolution]);
          }
        }
      });
    }

    // the aspect row alone, for the Advanced fold under the resolution it filters
    void render_aspect_control() {
      render_aspect_row();
    }

    // a stream preset sets a fixed size: the same path the Resolution row takes, so the dropdown follows it
    void apply_preset_resolution(int width, int height) {
      resolution_system_t::resolution_t resolution;
      resolution.width = (std::uint32_t)width;
      resolution.height = (std::uint32_t)height;
      resolution.name = std::to_string(width) + "x" + std::to_string(height);
      resolution.category = "Preset";
      apply_resolution(resolution);
    }

  private:
    void render_aspect_row() {
      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt) {
        return;
      }
      uint32_t current_height = rt->screen_encoder.config_.height;
      auto aspect_options = rt->screen_encoder.resolution_manager.get_aspect_ratio_options();
      std::vector<std::string> aspect_strings;
      std::vector<const char*> aspect_cstrings;
      for (const auto& [name, ratio] : aspect_options) {
        aspect_strings.push_back(name);
      }
      for (const auto& str : aspect_strings) {
        aspect_cstrings.push_back(str.c_str());
      }
      gui::settings_row("Aspect", [&] {
        if (gui::combo("##aspect_ratio", &selected_aspect_ratio, aspect_cstrings.data(), aspect_cstrings.size())) {
          apply_closest_resolution(current_height);
        }
      });
    }

    std::vector<resolution_system_t::resolution_t> get_filtered_resolutions() {
      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt) return {};
      if (selected_aspect_ratio == 4) {
        return rt->screen_encoder.resolution_manager.detected_info.matching_aspect_resolutions;
      }
      else {
        auto aspect_options = rt->screen_encoder.resolution_manager.get_aspect_ratio_options();
        f32_t selected_aspect = aspect_options[selected_aspect_ratio].second;
        return resolution_system_t::get_resolutions_by_aspect(selected_aspect, 0.01f);
      }
    }

    void sync_dropdown_to_current_resolution(uint32_t current_width, uint32_t current_height) {
      if (current_width == 0 || current_height == 0) return;

      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt) return;

      f32_t current_aspect = static_cast<f32_t>(current_width) / current_height;
      auto aspect_options = rt->screen_encoder.resolution_manager.get_aspect_ratio_options();

      // auto can match the same ratio as a named option, keep what the user picked
      if (std::abs(aspect_options[selected_aspect_ratio].second - current_aspect) >= 0.02f) {
        for (int i = 0; i < aspect_options.size(); i++) {
          if (std::abs(aspect_options[i].second - current_aspect) < 0.02f) {
            selected_aspect_ratio = i;
            break;
          }
        }
      }

      auto available_resolutions = get_filtered_resolutions();
      selected_resolution = 0;

      for (int i = 0; i < available_resolutions.size(); i++) {
        if (available_resolutions[i].width == current_width &&
          available_resolutions[i].height == current_height) {
          selected_resolution = i;
          break;
        }
      }
    }

    void apply_closest_resolution(uint32_t current_height) {
      auto resolutions = get_filtered_resolutions();
      auto height_distance = [&](const auto& resolution) {
        return std::abs((int)resolution.height - (int)current_height);
      };
      auto closest = std::min_element(resolutions.begin(), resolutions.end(), [&](const auto& a, const auto& b) {
        return height_distance(a) < height_distance(b);
      });
      if (closest != resolutions.end()) {
        apply_resolution(*closest);
      }
    }

    void apply_resolution(const resolution_system_t::resolution_t& resolution) {
      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (rt) {
        rt->screen_encoder.set_user_resolution(resolution.width, resolution.height);
        rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;

        if (!rt->ecps_gui.lossless && rt->ecps_gui.stream_settings.bitrate_mode == 0) {
          rt->screen_encoder.config_.bitrate = dynamic_config_t::get_adaptive_bitrate();
          {
            std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
            rt->screen_encoder.update_flags |= codec_update_e::rate_control;
          }
        }

        last_applied_width = resolution.width;
        last_applied_height = resolution.height;
      }
    }
  }resolution_controls;

#undef This
#define This this
