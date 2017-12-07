#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include "../fhd.h"
#include "../fhd_candidate_db.h"
#include "../fhd_classifier.h"
#include "../fhd_image.h"
#include "../fhd_kinect.h"
#include "../fhd_math.h"
#include "../fhd_segmentation.h"
#include "../fhd_sqlite_source.h"
#include "../imgui/imgui.h"
#include "../imgui/imgui_impl_glfw.h"
#include "../pcg/pcg_basic.h"
#include "fhd_debug_frame_source.h"
#include "fhd_filebrowser.h"
#include "fhd_texture.h"
#include "fhd_training.h"

#if WIN32
#include "fhd_kinect_source.h"
#endif

struct fhd_color {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;

  fhd_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
      : r(r), g(g), b(b), a(a) {}
};

uint8_t depth_to_byte(uint16_t value, uint16_t min, uint16_t max) {
  const uint32_t v = fhd_clamp(value, min, max);
  const uint32_t lmin = min;
  const uint32_t lmax = max;
  return uint8_t((v - lmin) * 255 / (lmax - lmin));
}

void update_depth_texture(uint8_t* texture_data, const uint16_t* depth,
                          int len) {
  for (int i = 0; i < len; i++) {
    uint16_t reading = depth[i];
    uint8_t normalized = depth_to_byte(reading, 500, 4500);

    int idx = 4 * i;

    if (reading >= 500 && reading <= 4500) {
      texture_data[idx] = normalized;
      texture_data[idx + 1] = 255 - normalized;
      texture_data[idx + 2] = 255 - normalized;
    } else {
      texture_data[idx] = 0;
      texture_data[idx + 1] = 0;
      texture_data[idx + 2] = 0;
    }
    texture_data[idx + 3] = 255;
  }
}

enum selection_state {
  sel_state_deselected,
  sel_state_selected,
  sel_state_discard
};

struct fhd_ui {
  fhd_ui(fhd_context* fhd);

  fhd_context* fhd = NULL;
  fhd_classifier* classifier = NULL;
  fhd_filebrowser file_browser;
  std::unique_ptr<fhd_frame_source> frame_source;

  fhd_texture depth_texture;
  fhd_texture normals_texture;
  fhd_texture normals_seg_texture;
  fhd_texture downscaled_depth;
  fhd_texture depth_segmentation;
  fhd_texture filtered_regions;

  double detection_pass_time_ms = 0.0;
  bool update_enabled = true;
  bool show_candidates = false;
  bool show_file_selection = false;
  bool show_trainining_window = false;
  const uint16_t* depth_frame = NULL;
  float detection_threshold = 0.95f;
  int filebrowser_selected_index = -1;
  std::vector<fhd_texture> textures;
  std::vector<fhd_color> colors;
  std::vector<selection_state> selected_candidates;
  std::string source_database_name = "none";
  std::string classifier_name = "none";

  std::vector<fhd_image> candidate_images;
  fhd_candidate_db candidate_db;
  int numOutputCandidates = 0;

  fhd_training training;
};

fhd_ui::fhd_ui(fhd_context* fhd)
    : fhd(fhd),
      file_browser("."),
      depth_texture(fhd_create_texture(512, 424)),
      normals_texture(fhd_create_texture(fhd->cells_x, fhd->cells_y)),
      normals_seg_texture(fhd_create_texture(fhd->cells_x, fhd->cells_y)),
      downscaled_depth(fhd_create_texture(fhd->cells_x, fhd->cells_y)),
      depth_segmentation(fhd_create_texture(fhd->cells_x, fhd->cells_y)),
      filtered_regions(fhd_create_texture(fhd->cells_x, fhd->cells_y)),
      selected_candidates(fhd->candidates_capacity, sel_state_deselected),
      candidate_images(fhd->candidates_capacity) {
#if WIN32
  frame_source.reset(new fhd_kinect_source());
#else
  frame_source.reset(new fhd_debug_frame_source());
#endif

  for (int i = 0; i < fhd->candidates_capacity; i++) {
    fhd_texture t = fhd_create_texture(FHD_HOG_WIDTH, FHD_HOG_HEIGHT);
    textures.push_back(t);
    fhd_image_init(&candidate_images[i], FHD_HOG_WIDTH, FHD_HOG_HEIGHT);
  }

  for (int i = 0; i < fhd->cells_x * fhd->cells_y; i++) {
    uint8_t r = uint8_t(pcg32_boundedrand(255));
    uint8_t g = uint8_t(pcg32_boundedrand(255));
    uint8_t b = uint8_t(pcg32_boundedrand(255));
    colors.emplace_back(r, g, b);
  }
}

void fhd_ui_clear_candidate_selection(fhd_ui* ui) {
  for (size_t i = 0; i < ui->selected_candidates.size(); i++) {
    ui->selected_candidates[i] = sel_state_deselected;
  }
}

void fhd_ui_commit_candidates(fhd_ui* ui) {
  for (int i = 0; i < ui->fhd->candidates_len; i++) {
    selection_state selection = ui->selected_candidates[i];
    if (selection == sel_state_discard) {
      continue;
    }

    fhd_candidate* candidate = &ui->fhd->candidates[i];
    bool human = selection == sel_state_selected;
    fhd_candidate_db_add_candidate(&ui->candidate_db, candidate, human);
  }

  ui->numOutputCandidates = fhd_candidate_db_get_count(&ui->candidate_db);
}

void fhd_ui_update_candidates(fhd_ui* ui, const fhd_candidate* candidates,
                              int len) {
  for (int i = 0; i < len; i++) {
    const fhd_candidate* candidate = &candidates[i];
    fhd_image* candidate_img = &ui->candidate_images[i];

    fhd_image_region src_reg;
    src_reg.x = 1;
    src_reg.y = 1;
    src_reg.width = candidate->depth.width - 1;
    src_reg.height = candidate->depth.height - 1;

    fhd_image_region dst_reg;
    dst_reg.x = 0;
    dst_reg.y = 0;
    dst_reg.width = candidate_img->width;
    dst_reg.height = candidate_img->height;

    fhd_copy_sub_image(&candidate->depth, &src_reg, candidate_img, &dst_reg);
    update_depth_texture(ui->textures[i].data, candidate_img->data,
                         candidate_img->len);
    fhd_texture_update(&ui->textures[i], ui->textures[i].data);
  }
}

void fhd_ui_update(fhd_ui* ui, const uint16_t* depth) {
  update_depth_texture(ui->depth_texture.data, depth, 512 * 424);
  fhd_texture_update(&ui->depth_texture, ui->depth_texture.data);

  fhd_context* fhd = ui->fhd;
  for (int i = 0; i < fhd->cells_len; i++) {
    const fhd_vec3 n = fhd->normals[i];
    ui->normals_texture.data[4 * i] = uint8_t(((1.f + n.x) * 0.5f) * 255.f);
    ui->normals_texture.data[4 * i + 1] = uint8_t(((1.f + n.y) * 0.5f) * 255.f);
    ui->normals_texture.data[4 * i + 2] = uint8_t(((1.f + n.z) * 0.5f) * 255.f);
    ui->normals_texture.data[4 * i + 3] = 255;
  }

  for (int i = 0; i < fhd->cells_len; i++) {
    int component = fhd_segmentation_find(fhd->normals_segmentation, i);

    fhd_color color = ui->colors[component];

    ui->normals_seg_texture.data[4 * i] = color.r;
    ui->normals_seg_texture.data[4 * i + 1] = color.g;
    ui->normals_seg_texture.data[4 * i + 2] = color.b;
    ui->normals_seg_texture.data[4 * i + 3] = 255;
  }

  for (int i = 0; i < fhd->cells_len; i++) {
    int component = fhd_segmentation_find(fhd->depth_segmentation, i);
    fhd_color color = ui->colors[component];

    ui->depth_segmentation.data[4 * i] = color.r;
    ui->depth_segmentation.data[4 * i + 1] = color.g;
    ui->depth_segmentation.data[4 * i + 2] = color.b;
    ui->depth_segmentation.data[4 * i + 3] = 255;
  }

  std::memset(ui->filtered_regions.data, 0, ui->filtered_regions.bytes);
  for (int i = 0; i < fhd->filtered_regions_len; i++) {
    fhd_region* r = &fhd->filtered_regions[i];
    fhd_color c = ui->colors[i];
    for (int j = 0; j < r->points_len; j++) {
      fhd_vec3 p = r->points[j].p_r;
      const fhd_vec2 sc = fhd_kinect_coord_to_depth(p);
      const int sx = int(std::round(sc.x / fhd->cell_wf));
      const int sy = int(std::round(sc.y / fhd->cell_hf));
      int idx = sy * fhd->cells_x + sx;
      ui->filtered_regions.data[4 * idx] = c.r;
      ui->filtered_regions.data[4 * idx + 1] = c.g;
      ui->filtered_regions.data[4 * idx + 2] = c.b;
      ui->filtered_regions.data[4 * idx + 3] = 255;
    }
  }

  update_depth_texture(ui->downscaled_depth.data, fhd->downscaled_depth,
                       fhd->cells_len);

  fhd_texture_upload(&ui->normals_texture);
  fhd_texture_upload(&ui->normals_seg_texture);
  fhd_texture_upload(&ui->downscaled_depth);
  fhd_texture_upload(&ui->depth_segmentation);
  fhd_texture_upload(&ui->filtered_regions);

  fhd_ui_update_candidates(ui, ui->fhd->candidates, ui->fhd->candidates_len);
}

void render_directory(fhd_ui* ui) {
  ImGui::BeginChild("files", ImVec2(400, 200), true);

  int current = ui->filebrowser_selected_index;
  int new_selection = -1;
  for (int i = 0; i < int(ui->file_browser.files.size()); i++) {
    const char* file = ui->file_browser.files[i].name.c_str();
    if (ImGui::Selectable(file, i == current)) {
      new_selection = i;
    }
  }

  ImGui::EndChild();

  if (const fhd_file* selected_file =
          ui->file_browser.get_file(new_selection)) {
    if (selected_file->is_dir) {
      ui->filebrowser_selected_index = -1;
    } else {
      ui->filebrowser_selected_index = new_selection;
    }

    ui->file_browser.set_root(selected_file->path.c_str());
  }
}

void render_db_selection(fhd_ui* ui) {
  if (ImGui::Button("open input database")) {
    ImGui::OpenPopup("select database");
  }

  if (ImGui::BeginPopupModal("select database", NULL,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    render_directory(ui);
    if (ImGui::Button("open")) {
      int path_index = ui->filebrowser_selected_index;
      const fhd_file* selected_file = ui->file_browser.get_file(path_index);
      if (selected_file) {
        ui->frame_source.reset(
            new fhd_sqlite_source(selected_file->path.c_str()));
        ui->frame_source->advance();
        ui->source_database_name = selected_file->name;
        ui->filebrowser_selected_index = -1;
      }

      ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("cancel")) {
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }
}

void render_classifier_selection(fhd_ui* ui) {
  if (ImGui::Button("open classifier")) {
    ImGui::OpenPopup("select classifier");
  }

  if (ImGui::BeginPopupModal("select classifier", NULL,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    render_directory(ui);

    if (ImGui::Button("open")) {
      int path_index = ui->filebrowser_selected_index;
      const fhd_file* selected_file = ui->file_browser.get_file(path_index);
      if (selected_file) {
        fhd_classifier_destroy(ui->classifier);

        ui->classifier =
            fhd_classifier_create(selected_file->path.c_str());
        if (ui->classifier) {
          ui->classifier_name = selected_file->name;
        } else {
          ui->classifier_name = "none";
        }
      }

      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }
}

void fhd_candidate_selection_grid(fhd_ui* ui, int btn_width, int btn_height) {
  const float bwidth = float(btn_width);
  const float bheight = float(btn_height);
  const ImVec2 size(bwidth, bheight);

  for (int i = 0; i < ui->fhd->candidates_len; i++) {
    fhd_texture* texture = &ui->textures[i];

    selection_state selection = ui->selected_candidates[i];

    void* handle = (void*)intptr_t(texture->handle);
    switch (selection) {
      case sel_state_selected: {
        if (ImGui::ImageButton(handle, size, ImVec2(0, 0), ImVec2(1, 1), 2,
                               ImVec4(1.f, 1.f, 1.f, 1.f),
                               ImVec4(0.f, 1.f, 0.f, 1.f))) {
          ui->selected_candidates[i] = sel_state_discard;
        }
        break;
      }
      case sel_state_discard: {
        if (ImGui::ImageButton(handle, size, ImVec2(0, 0), ImVec2(1, 1), 2,
                               ImVec4(1.f, 1.f, 1.f, 1.f),
                               ImVec4(1.f, 1.f, 0.f, 1.f))) {
          ui->selected_candidates[i] = sel_state_deselected;
        }
      } break;
      case sel_state_deselected: {
        if (ImGui::ImageButton(handle, size, ImVec2(0, 0), ImVec2(1, 1), 2)) {
          ui->selected_candidates[i] = sel_state_selected;
        }
        break;
      }
      default:
        break;
    }

    if (i % 7 < 6) ImGui::SameLine();
  }
}

void render_training_window(fhd_ui* ui) {
  if (!ui->show_trainining_window) return;

  fhd_training* training = &ui->training;
  ImGui::Begin("Training", &ui->show_trainining_window);
  ImGui::InputText("candidate database", &training->candidate_database_name[0],
                   training->candidate_database_name.capacity());
  ImGui::InputText("output classifier", &training->output_classifier_buf[0],
                   training->output_classifier_buf.size());
  if (training->running) {
    if (ImGui::Button("Cancel")) {
      training->running = false;
    }
  } else {
    if (ImGui::Button("Start training")) {
      fhd_training_start(&ui->training);
    }
  }
  ImGui::SameLine();
  std::unique_lock<std::mutex> lock(training->access_lock);
  if (!training->error.empty()) {
    ImGui::Text("%s", training->error.c_str());
  }
  ImGui::Text("Epoch %d", training->epoch.load());

  ImGui::PlotLines("MSE", training->errors.data(), int(training->errors.size()),
                   0, "error", 0.f, FLT_MAX, ImVec2(800.f, 300.f));
  lock.unlock();

  ImGui::End();
}

void glfwError(int error, const char* description) {
  fprintf(stderr, "GLFW error: %i: %s\n", error, description);
}

int main(int argc, char** argv) {
  glfwSetErrorCallback(glfwError);

  if (!glfwInit()) return 1;

  GLFWmonitor* monitor = glfwGetPrimaryMonitor();
  const GLFWvidmode* mode = glfwGetVideoMode(monitor);

  GLFWwindow* window =
      glfwCreateWindow(mode->width, mode->height, "HumanDetection", NULL, NULL);
  glfwMakeContextCurrent(window);

  fhd_context detector;
  fhd_context_init(&detector, 512, 424, 8, 8);

  ImGui_ImplGlfw_Init(window, true);
  ImGui::GetStyle().WindowRounding = 0.f;
  bool show_window = true;

  fhd_ui ui(&detector);

  if (argc > 1) {
    fhd_candidate_db_init(&ui.candidate_db, argv[1]);
  } else {
    fhd_candidate_db_init(&ui.candidate_db, "output.db");
  }

  ui.numOutputCandidates = fhd_candidate_db_get_count(&ui.candidate_db);

  ImVec4 clear_color = ImColor(218, 223, 225);
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    if (ImGui::IsKeyPressed(GLFW_KEY_ESCAPE)) break;

    if (ImGui::IsKeyPressed(GLFW_KEY_Q)) {
      ui.update_enabled ^= 1;
    }

    if (ImGui::IsKeyPressed(GLFW_KEY_X)) {
      fhd_ui_clear_candidate_selection(&ui);
      ui.depth_frame = ui.frame_source->get_frame();
      auto t1 = std::chrono::high_resolution_clock::now();
      fhd_run_pass(&detector, ui.depth_frame);

      if (ui.classifier) {
        fhd_run_classifier(ui.fhd, ui.classifier);  
      }

      auto t2 = std::chrono::high_resolution_clock::now();
      auto duration =
          std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);
      ui.detection_pass_time_ms = double(duration.count()) / 1000.0;
      fhd_ui_update(&ui, ui.depth_frame);
    }

    if (ImGui::IsKeyPressed(GLFW_KEY_SPACE)) {
      fhd_ui_commit_candidates(&ui);
    }

    if (ui.depth_frame && ui.update_enabled) {
      auto t1 = std::chrono::high_resolution_clock::now();
      fhd_run_pass(&detector, ui.depth_frame);
      auto t2 = std::chrono::high_resolution_clock::now();
      auto duration =
          std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);
      ui.detection_pass_time_ms = double(duration.count()) / 1000.0;
      fhd_ui_update(&ui, ui.depth_frame);
    }

    ImGui_ImplGlfw_NewFrame();
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);

    ImGui::SetNextWindowPos(ImVec2(0, 0));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoTitleBar;

    ImGui::Begin("##main", &show_window,
                 ImVec2(float(display_w), float(display_h)), -1.f, flags);

    ImGui::Text("Hotkeys: X - next frame, SPACE - commit");
    ImGui::Text("input: %s", ui.source_database_name.c_str());
    ImGui::Text("output: %s", fhd_candidate_db_name(&ui.candidate_db));
    ImGui::Text("classifier: %s", ui.classifier_name.c_str());
    ImGui::BeginChild("toolbar", ImVec2(300.f, float(display_h)));
    render_db_selection(&ui);
    render_classifier_selection(&ui);

    ImGui::Text("detection pass time %.3f ms", ui.detection_pass_time_ms);
    ImGui::Text("input frame %d/%d", ui.frame_source->current_frame(),
                ui.frame_source->total_frames());

    int jump_frame = ui.frame_source->current_frame();
    if (ImGui::InputInt("jump to frame", &jump_frame, 1, 100,
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
      ui.frame_source->jump(jump_frame);
      fhd_ui_clear_candidate_selection(&ui);
      ui.depth_frame = ui.frame_source->get_frame();
    }

    ImGui::Text("output candidates %d", ui.numOutputCandidates);
    ImGui::Checkbox("update enabled", &ui.update_enabled);
    ImGui::SliderFloat("##det_thresh", &ui.detection_threshold, -1.f, 1.f,
                       "detection threshold %.3f");
    ImGui::InputFloat("seg k depth", &ui.fhd->depth_segmentation_threshold);
    ui.fhd->depth_segmentation_threshold =
        std::max(0.1f, ui.fhd->depth_segmentation_threshold);
    ImGui::InputFloat("seg k normals", &ui.fhd->normal_segmentation_threshold);
    ImGui::SliderFloat("##min_reg_dim", &ui.fhd->min_region_size, 1.f, 100.f,
                       "min region dimension %.1f");
    ImGui::SliderFloat("##merge_dist_x", &ui.fhd->max_merge_distance, 0.1f, 2.f,
                       "max h merge dist (m) %.2f");
    ImGui::SliderFloat("##merge_dist_y", &ui.fhd->max_vertical_merge_distance,
                       0.1f, 3.f, "max v merge dist (m) %.2f");
    ImGui::SliderFloat("##min_inlier", &ui.fhd->min_inlier_fraction, 0.5f, 1.f,
                       "RANSAC min inlier ratio %.2f");
    ImGui::SliderFloat("##max_plane_dist", &ui.fhd->ransac_max_plane_distance,
                       0.01f, 1.f, "RANSAC max plane dist %.2f");
    ImGui::SliderFloat("##reg_height_min", &ui.fhd->min_region_height, 0.1f,
                       3.f, "min region height (m) %.2f");
    ImGui::SliderFloat("##reg_height_max", &ui.fhd->max_region_height, 0.1f,
                       3.f, "max region height (m) %.2f");
    ImGui::SliderFloat("##reg_width_min", &ui.fhd->min_region_width, 0.1f, 1.f,
                       "min region width (m) %.2f");
    ImGui::SliderFloat("##reg_width_max", &ui.fhd->max_region_width, 0.1f, 1.5f,
                       "max region width (m) %.2f");
    ImGui::SliderInt("##min_depth_seg_size", &ui.fhd->min_depth_segment_size, 1,
                     200, "min depth seg size %.0f");
    ImGui::SliderInt("##min_normal_seg_size", &ui.fhd->min_normal_segment_size,
                     1, 200, "min normal seg size %.0f");

    if (ImGui::Button("Training")) {
      ui.show_trainining_window ^= 1;
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginGroup();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Image((void*)intptr_t(ui.depth_texture.handle), ImVec2(512, 424));

    ImU32 rect_color = ImColor(240, 240, 20);
    for (int i = 0; i < detector.candidates_len; i++) {
      const fhd_candidate* candidate = &detector.candidates[i];
      if (candidate->weight >= ui.detection_threshold) {
        const fhd_image_region region = candidate->depth_position;

        const float x = p.x + float(region.x);
        const float y = p.y + float(region.y);
        const float w = float(region.width);
        const float h = float(region.height);
        ImVec2 points[4] = {ImVec2(x, y), ImVec2(x + w, y),
                            ImVec2(x + w, y + h), ImVec2(x, y + h)};
        draw_list->AddPolyline(points, 4, rect_color, true, 4.f, true);
      }
    }

    ImGui::BeginGroup();
    ImGui::Image((void*)intptr_t(ui.normals_texture.handle), ImVec2(256, 212));
    ImGui::SameLine();
    ImGui::Image((void*)intptr_t(ui.normals_seg_texture.handle),
                 ImVec2(256, 212));
    ImGui::EndGroup();

    ImGui::BeginGroup();
    ImGui::Image((void*)intptr_t(ui.downscaled_depth.handle), ImVec2(256, 212));
    ImGui::SameLine();
    ImGui::Image((void*)intptr_t(ui.depth_segmentation.handle),
                 ImVec2(256, 212));
    ImGui::EndGroup();

    ImGui::Image((void*)intptr_t(ui.filtered_regions.handle), ImVec2(256, 212));

    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup();

    fhd_candidate_selection_grid(&ui, FHD_HOG_WIDTH * 2, FHD_HOG_HEIGHT * 2);

    ImGui::EndGroup();

    render_training_window(&ui);

    ImGui::End();

    glViewport(0, 0, display_w, display_h);
    glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui::Render();
    glfwSwapBuffers(window);
  }

  fhd_candidate_db_close(&ui.candidate_db);

  fhd_texture_destroy(&ui.depth_texture);
  fhd_classifier_destroy(ui.classifier);
  ImGui_ImplGlfw_Shutdown();
  glfwTerminate();

  return 0;
}
