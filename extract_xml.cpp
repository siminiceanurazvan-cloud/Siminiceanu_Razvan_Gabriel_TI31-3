#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>

namespace fs = std::filesystem;

enum class ExtractMode {
    TitleAndText, // <title> + all <p> in <text> (default, standard for Reuters)
    TextOnly,     // Only <p> in <text>
    AllText       // Strips all XML tags across the entire file
};

// Helper: Trim leading and trailing whitespace
std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        start++;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        end--;
    }
    return s.substr(start, end - start);
}

// Helper: Case-insensitive search for substring
size_t find_case_insensitive(const std::string& text, const std::string& target, size_t pos = 0) {
    if (target.empty() || pos >= text.size()) return std::string::npos;
    auto it = std::search(
        text.begin() + pos, text.end(),
        target.begin(), target.end(),
        [](char ch1, char ch2) {
            return std::tolower(static_cast<unsigned char>(ch1)) == 
                   std::tolower(static_cast<unsigned char>(ch2));
        }
    );
    if (it == text.end()) return std::string::npos;
    return std::distance(text.begin(), it);
}

// Helper: Decode XML entities (&quot;, &amp;, &lt;, &gt;, &apos;, numeric)
std::string decode_xml_entities(const std::string& input) {
    std::string result;
    result.reserve(input.size());

    size_t i = 0;
    const size_t len = input.size();

    while (i < len) {
        if (input[i] == '&') {
            size_t semi = input.find(';', i);
            if (semi != std::string::npos && semi - i <= 10) {
                std::string entity = input.substr(i + 1, semi - (i + 1));
                if (entity == "quot") {
                    result += '"';
                    i = semi + 1;
                    continue;
                } else if (entity == "amp") {
                    result += '&';
                    i = semi + 1;
                    continue;
                } else if (entity == "apos") {
                    result += '\'';
                    i = semi + 1;
                    continue;
                } else if (entity == "lt") {
                    result += '<';
                    i = semi + 1;
                    continue;
                } else if (entity == "gt") {
                    result += '>';
                    i = semi + 1;
                    continue;
                } else if (!entity.empty() && entity[0] == '#') {
                    int codepoint = 0;
                    bool valid = false;
                    try {
                        if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X')) {
                            codepoint = std::stoi(entity.substr(2), nullptr, 16);
                            valid = true;
                        } else if (entity.size() > 1) {
                            codepoint = std::stoi(entity.substr(1), nullptr, 10);
                            valid = true;
                        }
                    } catch (...) {
                        valid = false;
                    }
                    if (valid && codepoint > 0 && codepoint <= 255) {
                        result += static_cast<char>(codepoint);
                        i = semi + 1;
                        continue;
                    }
                }
            }
        }
        result += input[i];
        i++;
    }

    return result;
}

// Helper: Strip XML tags from a snippet and decode entities
std::string strip_tags_and_decode(const std::string& input) {
    std::string stripped;
    stripped.reserve(input.size());

    bool inside_tag = false;
    for (char ch : input) {
        if (ch == '<') {
            inside_tag = true;
        } else if (ch == '>') {
            inside_tag = false;
        } else if (!inside_tag) {
            stripped += ch;
        }
    }

    return trim(decode_xml_entities(stripped));
}

// Helper: Extract text enclosed by <tag>...</tag>
bool extract_tag_content(const std::string& xml, const std::string& open_tag, const std::string& close_tag, size_t& search_pos, std::string& out_content) {
    size_t start = find_case_insensitive(xml, open_tag, search_pos);
    if (start == std::string::npos) {
        search_pos = xml.size();
        return false;
    }

    size_t content_start = start + open_tag.size();
    size_t end = find_case_insensitive(xml, close_tag, content_start);
    if (end == std::string::npos) {
        search_pos = xml.size();
        return false;
    }

    search_pos = end + close_tag.size();
    out_content = xml.substr(content_start, end - content_start);
    return true;
}

// Main extraction routine for an XML string
std::string extract_text_from_xml(const std::string& xml, ExtractMode mode) {
    if (mode == ExtractMode::AllText) {
        return strip_tags_and_decode(xml);
    }

    std::ostringstream out;

    // 1. Extract <title> if TitleAndText mode
    if (mode == ExtractMode::TitleAndText) {
        size_t pos = 0;
        std::string title;
        if (!extract_tag_content(xml, "<title>", "</title>", pos, title)) {
            pos = 0;
            extract_tag_content(xml, "<headline>", "</headline>", pos, title);
        }
        title = strip_tags_and_decode(title);
        if (!title.empty()) {
            out << title << "\n";
        }
    }

    // 2. Extract <text> block
    size_t text_pos = 0;
    std::string text_block;
    if (!extract_tag_content(xml, "<text>", "</text>", text_pos, text_block)) {
        // Fallback: search entire xml for <p> tags
        text_block = xml;
    }

    // 3. Extract all <p> paragraphs within the text block
    size_t p_pos = 0;
    bool found_p = false;
    std::string p_content;
    while (extract_tag_content(text_block, "<p>", "</p>", p_pos, p_content)) {
        std::string cleaned_p = strip_tags_and_decode(p_content);
        if (!cleaned_p.empty()) {
            out << cleaned_p << "\n";
            found_p = true;
        }
    }

    // If no <p> tags found, strip entire text block as fallback
    if (!found_p && !text_block.empty()) {
        std::string fallback = strip_tags_and_decode(text_block);
        if (!fallback.empty()) {
            out << fallback << "\n";
        }
    }

    return out.str();
}

// Check if a path has an XML extension (.xml or .XML)
bool is_xml_file(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".xml";
}

// Read entire file content into string
bool read_file(const fs::path& file_path, std::string& content) {
    std::ifstream file(file_path, std::ios::in | std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    content.resize(file.tellg());
    file.seekg(0, std::ios::beg);
    file.read(&content[0], content.size());
    return true;
}

// Write string to file
bool write_file(const fs::path& file_path, const std::string& content) {
    std::ofstream file(file_path, std::ios::out | std::ios::binary);
    if (!file) return false;
    file.write(content.data(), content.size());
    return true;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage:\n";
    std::cout << "  " << prog_name << " [options] [input_directory] [output_directory]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --title-and-text   Extract <title> and all <p> in <text> (default)\n";
    std::cout << "  --text-only        Extract only <p> paragraphs from <text>\n";
    std::cout << "  --all-text         Extract all text from document (strip all XML tags)\n";
    std::cout << "  --out <dir>        Save .txt files in <dir>, mirroring directory hierarchy\n";
    std::cout << "  --in-place         Save each .txt alongside its corresponding .xml (default)\n";
    std::cout << "  --help             Display this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << prog_name << "\n";
    std::cout << "  " << prog_name << " Reuters\n";
    std::cout << "  " << prog_name << " Reuters/Reuters_34\n";
    std::cout << "  " << prog_name << " Reuters --out extracted_txt\n";
}

int main(int argc, char* argv[]) {
    std::cout << "====================================================\n";
    std::cout << "       Reuters XML to TXT Extractor (C++17)         \n";
    std::cout << "====================================================\n\n";

    fs::path input_dir = "Reuters";
    fs::path output_dir = "";
    ExtractMode mode = ExtractMode::TitleAndText;
    bool in_place = true;

    // Parse command line arguments
    std::vector<std::string> positional_args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--title-and-text") {
            mode = ExtractMode::TitleAndText;
        } else if (arg == "--text-only") {
            mode = ExtractMode::TextOnly;
        } else if (arg == "--all-text") {
            mode = ExtractMode::AllText;
        } else if (arg == "--in-place") {
            in_place = true;
            output_dir = "";
        } else if (arg == "--out" && i + 1 < argc) {
            output_dir = argv[++i];
            in_place = false;
        } else if (!arg.empty() && arg[0] != '-') {
            positional_args.push_back(arg);
        } else {
            std::cerr << "Warning: Unknown option: " << arg << "\n";
        }
    }

    if (!positional_args.empty()) {
        input_dir = positional_args[0];
    }
    if (positional_args.size() > 1 && output_dir.empty()) {
        output_dir = positional_args[1];
        in_place = false;
    }

    // If run without arguments and default "Reuters" does not exist, ask user
    if (argc == 1 && !fs::exists(input_dir)) {
        std::cout << "Default directory 'Reuters' not found in current folder.\n";
        std::cout << "Enter path to folder containing XML files: ";
        std::string user_input;
        if (std::getline(std::cin, user_input) && !user_input.empty()) {
            input_dir = trim(user_input);
        }
    }

    if (!fs::exists(input_dir)) {
        std::cerr << "Error: Input directory does not exist: " << input_dir << "\n";
        return 1;
    }

    std::cout << "Input directory:  " << fs::absolute(input_dir).string() << "\n";
    if (in_place) {
        std::cout << "Output mode:      In-place (.txt alongside each .xml)\n";
    } else {
        std::cout << "Output directory: " << fs::absolute(output_dir).string() << "\n";
    }

    std::string mode_str = "Title + Text paragraphs (<p>)";
    if (mode == ExtractMode::TextOnly) mode_str = "Text paragraphs only (<p>)";
    else if (mode == ExtractMode::AllText) mode_str = "All text (XML stripped)";
    std::cout << "Extraction mode:  " << mode_str << "\n\n";

    // Scan for XML files
    std::cout << "Scanning for XML files...\n";
    std::vector<fs::path> xml_files;
    try {
        if (fs::is_directory(input_dir)) {
            for (const auto& entry : fs::recursive_directory_iterator(input_dir)) {
                if (entry.is_regular_file() && is_xml_file(entry.path())) {
                    xml_files.push_back(entry.path());
                }
            }
        } else if (fs::is_regular_file(input_dir) && is_xml_file(input_dir)) {
            xml_files.push_back(input_dir);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error scanning directory: " << e.what() << "\n";
        return 1;
    }

    const size_t total_files = xml_files.size();
    if (total_files == 0) {
        std::cout << "No XML files found in " << input_dir << ".\n";
        return 0;
    }

    std::cout << "Found " << total_files << " XML file(s) to process.\n\n";

    auto start_time = std::chrono::high_resolution_clock::now();

    size_t processed_count = 0;
    size_t error_count = 0;

    for (size_t i = 0; i < total_files; ++i) {
        const auto& xml_path = xml_files[i];

        // Determine output path
        fs::path out_path;
        if (in_place) {
            out_path = xml_path;
            out_path.replace_extension(".txt");
        } else {
            fs::path rel_path = fs::relative(xml_path, input_dir);
            out_path = output_dir / rel_path;
            out_path.replace_extension(".txt");
            std::error_code ec;
            fs::create_directories(out_path.parent_path(), ec);
        }

        std::string content;
        if (!read_file(xml_path, content)) {
            std::cerr << "\nFailed to read: " << xml_path.string() << "\n";
            error_count++;
            continue;
        }

        std::string extracted = extract_text_from_xml(content, mode);

        if (!write_file(out_path, extracted)) {
            std::cerr << "\nFailed to write: " << out_path.string() << "\n";
            error_count++;
            continue;
        }

        processed_count++;

        // Progress display: update every 100 files or at 100%
        if ((i + 1) % 100 == 0 || i + 1 == total_files) {
            double percent = (static_cast<double>(i + 1) / total_files) * 100.0;
            std::cout << "\rProcessing: " << (i + 1) << " / " << total_files 
                      << " [" << std::fixed << std::setprecision(1) << percent << "%]" << std::flush;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << "\n\n----------------------------------------------------\n";
    std::cout << "Extraction Complete!\n";
    std::cout << "Successfully processed: " << processed_count << " file(s)\n";
    if (error_count > 0) {
        std::cout << "Errors encountered:     " << error_count << " file(s)\n";
    }
    std::cout << "Elapsed time:           " << duration_ms << " ms (" 
              << std::fixed << std::setprecision(2) << (duration_ms / 1000.0) << " seconds)\n";
    std::cout << "----------------------------------------------------\n";

    return 0;
}
