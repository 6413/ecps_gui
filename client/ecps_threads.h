// What ecps.cpp's main starts, each in its own source so an edit to one recompiles only that one
#pragma once

void run_render_thread(std::promise<void>& render_thread_promise);  // ecps_threads.cpp
void run_encode_thread();                                         // ecps_encode.cpp
void run_decode_thread();                                         // ecps_threads.cpp
fan::event::task_t idle_task();                                   // ecps_threads.cpp
fan::event::runv_t<bool> network_tick();                          // ecps_threads.cpp
void decode_frame(render_thread_t* rt, std::vector<uint8_t>& frame_data); // ecps_backend.cpp
