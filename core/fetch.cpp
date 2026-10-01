#include "fetch.h"

#include "appevent.h"
#include "config.h"
#include "fileio.h"
#include "log.h"
#include "path.h"
#include "stringutil.h"

#include <curl/curl.h>
#undef min
#undef max

namespace sgd {

namespace {

constexpr auto sgdPrefix = "sgd://";

// sgd:// URLs are always resolved from local disk, never fetched. Search order:
//   1. config var sgd.assetsDir
//   2. environment variable LIBSGD_ASSETS
//   3. ~/.sgd/assets
String g_assetsDir;
auto g_assetsDirInit = configVarChanged("sgd.assetsDir").connect(nullptr, [](CString value) { g_assetsDir = value; });

Path g_defaultAssetsDir("~/.sgd/assets");

Expected<Path, FileioEx> findSgdAsset(CString url) {
	auto rel = Path(url.substr(6));

	std::vector<Path> dirs;
	if (!g_assetsDir.empty()) dirs.emplace_back(g_assetsDir);
	if (auto env = std::getenv("LIBSGD_ASSETS"); env && *env) dirs.emplace_back(env);
	dirs.push_back(g_defaultAssetsDir);

	String tried;
	for (auto& dir : dirs) {
		auto path = dir / rel;
		if (path.isFile()) return path;
		tried += "\n  " + path.str();
	}

	return SGD_FILEIOEX("Asset not found: " + String(url) + "\nSet LIBSGD_ASSETS or config var sgd.assetsDir. Looked in:" + tried);
}

CURL* g_curl;

CURL* openCurl() {
	if (g_curl) return g_curl;

	g_curl = curl_easy_init();
	if (!g_curl) {
		SGD_ERROR("!!! Failed to create CURL instance");
		return nullptr;
	}

	// Important! Should be the default!
	curl_easy_setopt(g_curl, CURLOPT_ACCEPT_ENCODING, "");

	// curl_easy_setopt(g_curl, CURLOPT_VERBOSE, 1);

	return g_curl;
}

Expected<bool, FileioEx> fetch(CString url, curl_write_callback writeFunc, void* writeData) {
	auto curl = openCurl();

	curl_easy_setopt(curl, CURLOPT_URL, String(url).c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunc);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, writeData);

	auto result = curl_easy_perform(curl);
	if (result != CURLcode::CURLE_OK) return SGD_FILEIOEX("CURL error: " + toString(curl_easy_strerror(result)));

	// CURLcode::CURLE_OK
	long httpStatus = -1;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
	if (httpStatus >= 200 && httpStatus < 300) return true;

	return SGD_FILEIOEX("CURL http request failed: " + toString(httpStatus));
}

} // namespace

Expected<String, FileioEx> fetchString(CString url) {
	if (startsWith(url, sgdPrefix)) {
		auto path = findSgdAsset(url);
		if (!path) return path.error();
		return loadString(path.result());
	}

	auto writeFunc = [](char* buffer, size_t size, size_t nmemb, void* datap) {
		size *= nmemb;
		auto text = (String*)datap;
		// FIXME: This is probably way less than optimal!
		text->append(buffer, size);
		return size;
	};

	String result;
	auto r = fetch(url, writeFunc, &result);
	if (!r) return r.error();

	return result;
}

Expected<Data, FileioEx> fetchData(CString url) {
	if (startsWith(url, sgdPrefix)) {
		auto path = findSgdAsset(url);
		if (!path) return path.error();
		return loadData(path.result());
	}

	auto writeFunc = [](char* buffer, size_t size, size_t nmemb, void* datap) {
		size *= nmemb;
		auto data = (Data*)datap;
		auto datasz = data->size();
		// FIXME: This is way less than optimal!
		data->resize(datasz + size);
		std::memcpy(data->data() + datasz, buffer, size);
		return size;
	};

	Data result;
	auto r = fetch(url, writeFunc, &result);
	if (!r) return r.error();

	return result;
}

} // namespace sgd
