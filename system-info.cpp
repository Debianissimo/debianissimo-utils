#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <regex>
#include <getopt.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>

/**
 * EXIT_SUCCESS = 0
 * EXIT_FAILURE = 1
 * 
 * Those were defined into stdlib.h
 */

#define EXIT_NOTFOUND 2
#define EXIT_FAIL     3


using namespace std;

bool debug = false;

// We're keeping the same paths for compatibility
const string CACHE_DIR = "/var/cache/oinfo";
const string CACHE_FILE = "oinfo.cache";

const string OS_RELEASE = "/etc/os-release";
const string ORDATA_DIR = "/etc/ordissimo";
const string ORDATA_PREFIX= "ORDISSIMO_";

// We're also keeping similar log messages for compatibility
void logDebug(const string &msg) {
    if(debug)
        cerr << "D: " << msg << endl;
}

void logWarn(const string &msg) {
    cerr << "[WARN] " << msg << endl;
}

void logError(const string &msg) {
    cerr << "[ERROR] " << msg << endl;
}

[[noreturn]] void logFatal(const string &msg) {
    cerr << "[FATAL] " << msg << endl;
    exit(EXIT_FAILURE);
}

string toLower(const string &s) {
    string res = s;
    transform(res.begin(), res.end(), res.begin(), ::tolower);
    return res;
}

string toUpper(const string &s) {
    string res = s;
    transform(res.begin(), res.end(), res.begin(), ::toupper);
    return res;
}

bool parseFile(const string &filePath, const function<void(const string &line)> &processor) {
    ifstream infile(filePath);
    if (!infile)
        return false;
    
    string line;
    while (getline(infile, line)) {
        if (line.empty())
            continue;
        processor(line);
    }
    infile.close();
    return true;
}

// Process ORDATA files in the directory
map<string, string> processOrdata() {
    map<string, string> ret;
    bool invalid_ordata = false;
    bool found_ordata = false;
    
    DIR *dir = opendir(ORDATA_DIR.c_str());
    if (!dir)
       logFatal("Directory " + ORDATA_DIR + " does not exist");
    
    struct dirent *entry;
    vector<string> files;
    while ((entry = readdir(dir)) != nullptr) {
        // skip . and ..
        if (entry->d_name[0] == '.')
            continue;
        
        files.push_back(entry->d_name);
    }

    sort(files.begin(), files.end());
    
    for (const auto &file : files) {
        string filepath = ORDATA_DIR + "/" + file;

        bool fileProcessed = parseFile(filepath, [&](const string &line) {
            istringstream iss(line);
            string token;
            if (!(iss >> token))
                return;
            if (token != "export") {
                invalid_ordata = true;
                return;
            }
            string var;
            if (!(iss >> var))
                return;
            if (var.find(ORDATA_PREFIX) != 0) {
                invalid_ordata = true;
                return;
            }
            size_t eq = var.find('=');
            if (eq == string::npos)
                return;
            
            string key = var.substr(ORDATA_PREFIX.size(), eq - ORDATA_PREFIX.size());
            string value = var.substr(eq + 1);
            ret[toLower(key)] = value;
            found_ordata = true;
        });
        
        if (!fileProcessed)
            logWarn("Couldn't open " + filepath);
    }
    closedir(dir);
    
    if (!found_ordata)
       logFatal("Couldn't read files in " + ORDATA_DIR + "/");
    
    if (invalid_ordata)
       logWarn("Invalid data found in " + ORDATA_DIR + "/");
    
    return ret;
}

map<string, string> processOSRelease() {
    map<string, string> ret;
    bool invalid_osrelease = false;
    
    bool fileProcessed = parseFile(OS_RELEASE, [&](const string &line) {
        // Ensure the line contains an '='
        size_t eq = line.find('=');
        if (eq == string::npos) {
            invalid_osrelease = true;
            return;
        }
        string key = line.substr(0, eq);
        string value = line.substr(eq + 1);
        
        // If the value is enclosed in quotes, remove them.
        if (!value.empty() && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        ret["os_" + toLower(key)] = value;
    });
    
    if (!fileProcessed)
        logFatal("File " + OS_RELEASE + " does not exist");
    
    if (invalid_osrelease)
        logWarn("Invalid data found in " + OS_RELEASE);
    
    return ret;
}

// This function “gathers” the infos by reading the files in ORDATA_DIR and OS_RELEASE.
map<string, string> getAllData() {
    map<string, string> data = processOrdata();
    map<string, string> os_data = processOSRelease();
    data.insert(os_data.begin(), os_data.end());

    if (data.find("custom") == data.end())
        data["custom"] = "none";

    if (data.find("dev") == data.end())
        data["dev"] = "false";

    return data;
}

// Helper: get modification time of a file (or directory)
time_t getMtime(const string &path) {
    struct stat st;
    if(stat(path.c_str(), &st) == 0)
        return st.st_mtime;

    return 0;
}

map<string, int> readCache(const string &cache_file) {
    map<string, int> cache;
    ifstream infile(cache_file);
    if (!infile)
        return cache;

    string line;
    while(getline(infile, line)) {
        if (line.empty())
            continue;
        
            const string prefix = "declare -A cache=";
    if (line.rfind(prefix, 0) == 0) {
        line.erase(0, prefix.size());
    }
    while (!line.empty() && line.front() == '(') {
        line.erase(line.begin());
    }
    while (!line.empty() && line.back() == ')') {
        line.pop_back();
    }

    regex entryPattern(R"(\[\s*([^\]]+)\s*\]\s*=\s*\"([^\"]*)\")");
    smatch match;
    string::const_iterator searchStart(line.cbegin());

    while (regex_search(searchStart, line.cend(), match, entryPattern)) {
        string key = match[1].str();
        int value = stoi(match[2].str());
        
        cache[key] = value;

        // Move searchStart forward to continue parsing remaining key-value pairs
        searchStart = match.suffix().first;
    }
    }
    infile.close();
    return cache;
}

// Helper: Write a simple cache file (format: each line "key=value")
void writeCache(const string &cache_file, const map<string, int> &cache) {
    ofstream outfile(cache_file);
    if (!outfile) {
        logError("Cannot write cache '" + cache_file + "'");
        return;
    }
    outfile << "declare -A cache=(";
    for (map<string, int>::const_iterator it = cache.begin(); it != cache.end(); ++it) {
        outfile << "[" << it->first << "]=\"" << it->second << "\" ";
    }
    outfile << ")";
    outfile.close();
    // Set permissions to 0777
    chmod(cache_file.c_str(), 0777);
}

void showHelp(string program) {
    cout << "Debianissimo System Info Tool" << endl
         << "Retrieve system information." << endl
         << endl;

    cout << "Usage:" << endl
         << "  " << program << " --help | -h" << endl
         << "     Show this help message" << endl
         << endl;

    cout << "  " << program << " --list" << endl
         << "     Returns a list of available keys" << endl
         << endl;

    cout << "  " << program << " [--keys | --sh | --sh-export] KEY..." << endl
         << "     Display values for specified keys" << endl
         << "     Use 'all' to show all keys" << endl
         << "     --keys      = Outputs data as `KEY=VALUE`" << endl
         << "     --sh        = Outputs data as `OINFO_KEY=\"VALUE\"`" << endl
         << "     --sh-export = Outputs data as `export OINFO_KEY=\"VALUE\"`" << endl
         << endl;

    cout << "  " << program << " [-q] [--and | --or] (is|isnot)-KEY-VALUE..." << endl
         << "     Test if a key matches a value" << endl
         << "     Default operator: --or (use --and to change)" << endl
         << "     -q = Quiet. Suppress the output." << endl << endl;
}

void parseArgs(vector<string> &args, vector<string> &keys, vector<string> &tests) {
    regex key_regex("^[a-z_]+$");
    regex test_regex("^(is|isnot)-[a-z_]+-(?:\"[^\"]+\"|[^\"]+)$");
    for (auto &arg : args) {
        string lower = toLower(arg);

        if (lower.find("is-") == 0 || lower.find("isnot-") == 0) {
            if (!keys.empty()) {
                logFatal("Mutually exclusive actions: cannot mix '(is|isnot)-KEY-VALUE' and 'KEY'");
            }
            if (!regex_match(lower, test_regex))
                logFatal("Invalid argument (" + arg + ")");

            if (find(tests.begin(), tests.end(), lower) == tests.end())
                tests.push_back(lower);
        } else {
            if (!tests.empty()) {
                logFatal("Mutually exclusive actions: cannot mix '(is|isnot)-KEY-VALUE' and 'KEY'");
            }
            if (!regex_match(lower, key_regex))
                logFatal("Invalid key (" + arg + ")");

            if (find(keys.begin(), keys.end(), lower) == keys.end())
                keys.push_back(lower);
        }
    }
}

time_t getMaxMtime() {
    time_t ret = 0;
    time_t mtime = getMtime(ORDATA_DIR);
    if (mtime > ret)
        ret = mtime;


    mtime = getMtime(OS_RELEASE);
    if (mtime > ret)
        ret = mtime;

    DIR* ordir = opendir(ORDATA_DIR.c_str());
    if (ordir) {
        struct dirent *entry;
        while ((entry = readdir(ordir)) != nullptr) {
            if (entry->d_name[0] == '.')
                continue;
            string filepath = ORDATA_DIR + "/" + entry->d_name;

            mtime = getMtime(filepath);
            if (mtime > ret)
                ret = mtime;
        }
        closedir(ordir);
    }
    
    return ret;
}

void printTest(bool value) {
    if (value)
        cout << "yes";
    else
        cout << "no";

    cout << endl;
}

int outputValues(const vector<string> &keys_list, bool showKeys, bool fmtSh, bool fmtExport) {
    auto data = getAllData();
    // get all keys of infos
    vector<string> keys = keys_list;
    if (find(keys.begin(), keys.end(), "all") != keys.end()) {
        keys.clear();
        for (const auto &pair : data) {
            keys.push_back(pair.first);
        }
    }
    for (auto &k : keys) {
        if (data.find(k) == data.end()) {
            logError("Key '" + k + "' not found");
            return EXIT_NOTFOUND;
        }
        string var = data[k];
        if (fmtSh) {
            // put var between quotes and escape all "
            string oldVar = var;
            var = "\"";
            for (auto &c : oldVar) {
                if (c == '"')
                    var += "\\\"";
                else
                    var += c;
            }
            var += "\"";

            if (fmtExport) {
                cout << "export ";
            }
            cout << "OINFO_" << toUpper(k);
        } else if (showKeys) {
            cout << k;
        }
        if (fmtSh || showKeys) {
            cout << "=";
        }
        cout << var << endl;
    }

    return EXIT_SUCCESS;
}

int runTests(const vector<string> tests, bool _and, bool _or, bool quiet) {
    time_t maxMtime = getMaxMtime();
    string cacheFile;
    if (access(CACHE_DIR.c_str(), W_OK) == 0)
        cacheFile = CACHE_DIR;
    else {
        char* xdg = getenv("XDG_RUNTIME_DIR");
        if (xdg)
            cacheFile = string(xdg);
        else
            cacheFile = ".";
    }
    cacheFile += "/" + CACHE_FILE;

    map<string, int> cacheMap;
    struct stat cacheStat;
    bool cacheExists = (stat(cacheFile.c_str(), &cacheStat) == 0);
    string cachedKey = (_and ? "1" : "0") + string(_or ? "1" : "0");
    for (auto &t : tests)
        cachedKey += t + " ";
    if (!cachedKey.empty() && cachedKey.back() == ' ')
        cachedKey.pop_back();
    
    int code = 0;
    if (cacheExists) {
        if (cacheStat.st_mtime < maxMtime) {
            logDebug("Cache outdated");
            remove(cacheFile.c_str());
            cacheExists = false;
        } else {
            cacheMap = readCache(cacheFile);
            if (cacheMap.find(cachedKey) != cacheMap.end()) {
                logDebug("Cache hit");
                if (!quiet) {
                    printTest(cacheMap[cachedKey] == 0);
                }
                return cacheMap[cachedKey];
            }
        }
    }

    /**
     * At this point, we're sure that either:
     * - the cache file doesn't exist
     * - the cache file is outdated
     * - the cache file exists but the key wasn't there
     */
    auto data = getAllData();
    bool fail = true;
    for (auto &t : tests) {
        regex test_regex("^(is|isnot)-([^-]+)-(.+)$");
        smatch match;
        if (!regex_match(t, match, test_regex))
            logFatal("Parsing test '" + t + "' failed");

        string cond = match[1];
        string key = match[2];
        string val = match[3];
        // We're keeping the debug logs for compatibility reasons
        logDebug("Test: cond=" + cond + " key=" + key + " val=" + val);

        if (data.find(key) == data.end()) {
            logError("Key '" + key + "' not found");
            return EXIT_NOTFOUND;
        }

        bool matches = false;
        string info_val = toLower(data[key]);

        if (cond.find("is") == 0) {
            if (info_val == val) {
                matches = 1;
            }
            if (cond == "isnot") {
                matches = !matches;
            }
        }

        if (_and && !matches) {
            fail = true;
            break;
        }
        if (matches)
            fail = false;
    }

    if (code == EXIT_SUCCESS && fail)
        code = EXIT_FAIL;

    if (!quiet) {
        printTest(code == EXIT_SUCCESS);
    }

    if (code == EXIT_SUCCESS || code == EXIT_FAIL) {
        cacheMap[cachedKey] = code;
        writeCache(cacheFile, cacheMap);
        logDebug("Cache updated");
    }

    return code;
}

int main(int argc, char* argv[]) {
    bool quiet = false;
    bool outKeys = false;
    bool outSh = false;
    bool outExport = false;
    bool cmpAnd = false;
    bool cmpOr = false;
    int dbg = 0;
    vector<string> posArgs;  // positional arguments

    struct option long_options[] = {
      {"help",       no_argument,       0, 'h'},
      {"debug",      no_argument,       0, 'd'},
      {"keys",       no_argument,       0,  0 },
      {"sh",         no_argument,       0,  0 },
      {"sh-export",  no_argument,       0,  0 },
      {"and",        no_argument,       0,  0 },
      {"or",         no_argument,       0,  0 },
      {"list",       no_argument,       0,  0 },
      {"quiet",      no_argument,       0, 'q'},
      {0, 0, 0, 0}
    };

    int opt;
    int optIndex = 0;
    while ((opt = getopt_long(argc, argv, "hdq", long_options, &optIndex)) != -1) {
        switch(opt) {
            case 'h':
                showHelp(argv[0]);
                break;
            case 'd':
                dbg = 1;
                break;
            case 'q':
                quiet = true;
                break;
            case 0: {
                string name = long_options[optIndex].name;
                if (name == "keys")
                    outKeys = true;
                else if (name == "sh")
                    outSh = true;
                else if (name == "sh-export") {
                    outSh = true;
                    outExport = true;
                } else if (name == "and")
                    cmpAnd = true;
                else if (name == "or")
                    cmpOr = true;
                else if (name == "list") {
                    auto data = getAllData();
                    vector<string> allKeys;
                    for (const auto &pair : data)
                        allKeys.push_back(pair.first);

                    sort(allKeys.begin(), allKeys.end());
                    for (auto &k : allKeys)
                        cout << " - " << k << endl;
                    
                    return EXIT_SUCCESS;
                }
                break;
            }
            default:
                showHelp(argv[0]);
                return EXIT_FAILURE;
        }
    }
    

    if (cmpAnd && cmpOr)
        logFatal("Mutually exclusive options '--and' and '--or'");

    int fmtCount = (outKeys ? 1 : 0) + ((outSh || outExport) ? 1 : 0);
    if (fmtCount > 1)
        logFatal("Mutually exclusive options '--keys' and '--sh'/'--sh-export'");

    if (dbg)
        debug = true;

    for (int index = optind; index < argc; index++) {
        posArgs.push_back(argv[index]);
    }

    vector<string> keyList;
    vector<string> testList;
    if (posArgs.empty()) {
        showHelp(argv[0]);
        return EXIT_SUCCESS;
    }

    parseArgs(posArgs, keyList, testList);
    if (!keyList.empty()) {
        return outputValues(keyList, outKeys, outSh, outExport);
    }
    return runTests(testList, cmpAnd, cmpOr, quiet);
}
