#include <iostream>
#include <fstream>
#include <filesystem>
#include <shared_mutex>
#include <unordered_map>
#include <chrono>
#include <string>
#include <queue>
#include "httplib.h"
#include "json.hpp"
#include "circularBuffer.hpp"
// Map filename -> cache (content as std::string)
//std::unordered_map<std::string, std::string> file_caches;
std::unordered_map<std::string, CircularBuffer<std::string>> file_caches;

// Map filename -> queue of pending writes
std::unordered_map<std::string, CircularBuffer<std::string>> write_queues;

// Mutex to protect each file's cache and write queue
std::unordered_map<std::string, std::shared_mutex> cache_mutexes;
std::unordered_map<std::string, std::mutex> write_queue_mutexes;

// Condition variable to notify the background worker
std::condition_variable_any write_condition;

// Global mutex for the condition variable
std::mutex write_condition_mutex;

// Flag to stop the background worker
bool stop_worker = false;

using json = nlohmann::json;
//std::mutex file_mutex;
// Map filename -> shared_mutex
std::unordered_map<std::string, std::shared_mutex> file_mutexes;

// Mutex to protect the map itself
std::shared_mutex map_mutex;

//size of cahches
int cache_capacity = 1000;

//output file stream
std::ofstream myfile;

// Access mutex for a file, assuming it exists in file_mutexes
std::shared_mutex& get_mutex_for_file(const std::string& filename) {
    std::shared_lock<std::shared_mutex> lock(map_mutex);
    auto it = file_mutexes.find(filename);
    if (it == file_mutexes.end()) {
        throw std::runtime_error("Unknown file: " + filename);
    }
    return it->second;
}

int64_t current_time_ns() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}
void background_worker(const std::string& storage_dir) {
    while (true) {
        std::string filename;
        std::string data_to_write;
        std::string entry_str;

        // Wait for a write operation to be added to the queue
        {
            std::unique_lock<std::mutex> lock(write_condition_mutex);
            write_condition.wait(lock, [&]() {
                for (const auto& [file, queue] : write_queues) {
                    if (!queue.empty()) {
                        return true;
                    }
                }
                return stop_worker; // Exit if the worker is stopped
            });

            if (stop_worker) {
                break; // Exit the worker loop
            }

            // Find the first non-empty queue
            for (auto& [file, queue] : write_queues) {
                std::lock_guard<std::mutex> queue_lock(write_queue_mutexes[file]);
                if (!queue.empty()) {
                    filename = file;
                    
                    queue.pop(entry_str); 
                    //queue.pop();
                    break;
                }
            }
        }

        // Write the data to the file
        try {
            std::string path =  "/files/" + filename;
            //std::string tmp_path = path + ".tmp";
            std::ofstream ofs(path,std::ios::out | std::ios::app);

            if (!ofs) {
                std::cerr << "Failed to open file for writing: " << path << "\n";
                continue;
            }

            ofs << entry_str;
            ofs.close();

           // std::filesystem::rename(tmp_path, path);
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Failed to write file " << filename << ": " << e.what() << "\n";
        } catch (const std::exception& e) {
            std::cerr << "Unexpected error during file write for " << filename << ": " << e.what() << "\n";
        }
    }
}
// Load filenames from JSON config file into file_mutexes
bool load_config(const std::string& config_path) {
    std::ifstream ifs(config_path);
    if (!ifs) {
        std::cerr << "Failed to open config file: " << config_path << "\n";
        return false;
    }

    json config_json;
    try {
        ifs >> config_json;
    } catch (const std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << "\n";
        return false;
    }

    if (!config_json.is_array()) {
        std::cerr << "Config JSON is not an array of filenames\n";
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(map_mutex);
    for (const auto& filename_json : config_json) {
        if (!filename_json.is_string()) continue;
        std::string filename = filename_json.get<std::string>();

        // Insert if missing

        file_mutexes.try_emplace(filename);
         // Insert if missing
        /* auto [it, inserted] = file_mutexes.try_emplace(filename);
         if (inserted) {
             std::cerr << "Added mutex for file: " << filename << "\n";
         } else {
             std::cerr << "Mutex already exists for file: " << filename << "\n";
         }*/
        file_caches.emplace(filename, CircularBuffer<std::string>(cache_capacity));
        write_queues.emplace(filename, CircularBuffer<std::string>(cache_capacity));

        cache_mutexes.try_emplace(filename);
        write_queue_mutexes.try_emplace(filename);
        //write_queues.try_emplace(filename);
        const std::string storage_dir = "./";
        std::string path =  "/files/" + filename;
        // Use the per-file mutex for thread safety
       // std::shared_mutex& mtx = it->second;
      /*  std::shared_mutex* mtx = nullptr;
        try {
            mtx = &get_mutex_for_file(filename);
        } catch (...) {
            std::cerr << "Failed to retrieve mutex for file: " << filename << "\n";
            return false;
        }*/
       // std::shared_lock<std::shared_mutex> file_lock(mtx);
       /* std::shared_mutex* mtx = nullptr;
        try {
            mtx = &get_mutex_for_file(filename);
        } catch (...) {
            
            return false;
        }
        std::shared_lock<std::shared_mutex> lock(*mtx);
        */
        std::ifstream file_ifs(path);
        if (!file_ifs) {
            std::cerr << "Failed to open file: " << path << "\n";
            continue; // Skip this file and move to the next
        }

        // Read the entire file content into a string
        std::string content((std::istreambuf_iterator<char>(file_ifs)), std::istreambuf_iterator<char>());

        // Store the content in the cache
        {
            std::unique_lock<std::shared_mutex> cache_lock(cache_mutexes[filename]);
            //file_caches[filename] = content;
           // file_caches[filename].push(content);
           auto it = file_caches.find(filename);
            if (it == file_caches.end()) {
                return false;
            }

            it->second.push(content);
        }

        std::cerr << "Loaded file into cache: " << filename << "\n";
    }

    
    return true;
}
int main() {

    httplib::Server server;
    const std::string storage_dir = "./";
    const std::string config_path = "files.json";
    myfile.open ("/log_files/error_log.txt");
    //myfile << "Started.\n";

    if (!load_config(config_path)) {
        return 1;
    }

    // POST /write/<filename>
    server.Post(R"(/write/([\w\-.]+))", [&](const httplib::Request& req, httplib::Response& res) {
        std::string filename = req.matches[1];
    
        try {
            auto input_json = json::parse(req.body);
    
            if (!input_json.contains("sent_at") || !input_json["sent_at"].is_number_integer()) {
                res.status = 400;
                res.set_content(R"({"error":"Missing or invalid 'sent_at'"})", "application/json");
                return;
            }
    
            if (!input_json.contains("values") || !input_json["values"].is_array()) {
                res.status = 400;
                res.set_content(R"({"error":"Missing or invalid 'values'"})", "application/json");
                return;
            }
    
            if (!input_json.contains("client_id") || !input_json["client_id"].is_string()) {
                res.status = 400;
                res.set_content(R"({"error":"Missing or invalid 'client_id'"})", "application/json");
                return;
            }
    
            input_json["received_at"] = current_time_ns();
            std::string json_output = input_json.dump(4);
    
            // Update the cache for the specific file
            {
                std::unique_lock<std::shared_mutex> cache_lock(cache_mutexes[filename]);
                //file_caches[filename] = json_output;
                //file_caches[filename].push(json_output);
                auto it = file_caches.find(filename);
                if (it == file_caches.end()) {
                    res.status = 404;
                    res.set_content(R"({"error":"File not found"})", "application/json");
                    return;
                }
                std::cerr << "Caching entry for " << filename << ": " << json_output << "\n";

                it->second.push(json_output);
            }
    
            // Add the write operation to the queue
            {
                std::lock_guard<std::mutex> queue_lock(write_queue_mutexes[filename]);
                //write_queues[filename].push(json_output);
                auto it = write_queues.find(filename);
                if (it == write_queues.end()) {
                    res.status = 404;
                    res.set_content(R"({"error":"File not found"})", "application/json");
                    return;
                }
                std::cerr << "Queuing entry for " << filename << ": " << json_output << "\n";
                //myfile << "Writing this to a file.\n";
                if(it->second.is_full()){
                    myfile << "Warning: Write queue for " << filename << " is full. Overwriting oldest entries.\n";
                }
                //myfile << "Testing.\n";
                it->second.push(json_output);
            }
            write_condition.notify_all(); // Notify the background worker
            std::cerr << "POST request received for file: " << filename << "\n";

            res.set_content(json_output, "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(R"({"error":"Invalid JSON"})", "application/json");
        }
    });

    // GET /read/<filename>
    server.Get(R"(/read/([\w\-.]+))", [&](const httplib::Request& req, httplib::Response& res) {
        std::string filename = req.matches[1];
        std::cerr << "GET request received for file: " << filename << "\n";
        // How many last entries to read?
        // Default value
        int k = 1;
    
        // Check query parameter "last"
        if (req.has_param("last")) {
            try {
                k = std::stoi(req.get_param_value("last"));
            } catch (const std::exception& e) {
                k = 1;  // fallback
            }
        }
        

        

        // Find mutex (safe even if you pre-created everything)
        auto m_it = cache_mutexes.find(filename);
        if (m_it == cache_mutexes.end()) {
            res.status = 404;
            res.set_content(R"({"error":"File not found"})", "application/json");
            return;
        }
    
        // Lock shared (peek doesn't modify buffer)
        std::shared_lock<std::shared_mutex> cache_lock(m_it->second);
    
        // Find circular buffer
        auto it = file_caches.find(filename);
        if (it == file_caches.end()) {
            res.status = 404;
            res.set_content(R"({"error":"File not found"})", "application/json");
            return;
        }
        //make sure k is within bounds
        k = std::max(1, std::min(k,  static_cast<int>(it->second.size()))); // clamp between 1 and cache_capacity
        if (k == 1) {
            std::string entry_str;
            if (!it->second.peek(entry_str,k)) {
                res.status = 404;  
                res.set_content(R"({"error":"Buffer empty"})", "application/json");
                return;
            }
            json entry_json = json::parse(entry_str);
            std::cerr << "Sending GET response for " << filename << ": " << entry_json.dump() << "\n";
            res.set_content(entry_json.dump(), "application/json");
        }
        else{
            json j;
            j["entries"] = json::array();  // initialize array
            for (int k_idx = k; k_idx > 0; k_idx--) {
                std::string entry_str;
                if (!it->second.peek(entry_str, k_idx)) {
                    res.status = 404;  
                    res.set_content(R"({"error":"Buffer empty"})", "application/json");
                    return;
                }
                // Parse each string into JSON
                json entry_json = json::parse(entry_str);
                j["entries"].push_back(entry_json);
                

            }
            j["count"] = j["entries"].size();
            
            std::cerr << "Sending GET response for k>1 " << filename << "\n";
            res.set_content(j.dump(), "application/json");
        }
    });
    
    

    std::cout << "Server running at http://localhost:8080\n";
    // Start the background worker
    std::thread worker_thread(background_worker, storage_dir);
    server.listen("172.28.1.10", 8080);
    // Stop the worker gracefully on shutdown
    stop_worker = true;
    write_condition.notify_all();
    worker_thread.join();
    // Close error log file
    myfile.close();
}
