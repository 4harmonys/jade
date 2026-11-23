#pragma once

#include "log.hpp"
#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

using json = nlohmann::json;

class AnthropicClient {
  public:
	explicit AnthropicClient(std::string apiKey, std::string model = "claude-sonnet-5-5")
	    : apiKey(std::move(apiKey)), model(std::move(model)) {}

	std::string ask(const std::string &prompt, int maximumTokens = 1024) {
		json requestBody = {{"model", model},
		                    {"max_tokens", maximumTokens},
		                    {"messages", json::array({{{"role", "user"}, {"content", prompt}}})}};

		Log("ai", "sending to %s (max_tokens=%d, prompt=%zu chars)", model.c_str(), maximumTokens, prompt.size());
		json responseBody = post("/v1/messages", requestBody);

		Log("ai", "model responded: stop_reason=%s",
		    responseBody.contains("stop_reason") ? responseBody["stop_reason"].dump().c_str() : "?");
		if(responseBody.contains("usage")) {
			Log("ai", "usage %s", responseBody["usage"].dump().c_str());
		}

		if(responseBody.contains("error")) {
			throw std::runtime_error("api error: " + responseBody["error"].dump().substr(0, 500));
		}

		std::string text;
		if(responseBody.contains("content") && responseBody["content"].is_array()) {
			for(const json &block : responseBody["content"]) {
				if(block.contains("type") && block["type"] == "text" && block.contains("text") &&
				   block["text"].is_string()) {
					text += block["text"].get<std::string>();
				}
			}
		}
		if(text.empty()) {
			throw std::runtime_error("no text in response: " + responseBody.dump().substr(0, 500));
		}
		Log("ai", "got %zu chars of modified IR", text.size());
		return text;
	}

  private:
	std::string apiKey;
	std::string model;

	static size_t writeCallback(char *incomingData, size_t elementSize, size_t elementCount, void *userPointer) {
		size_t totalSize = elementSize * elementCount;
		static_cast<std::string *>(userPointer)->append(incomingData, totalSize);
		return totalSize;
	}

	json post(const std::string &path, const json &requestBody) {
		std::string requestText = requestBody.dump();
		std::string responseText;

		CURL *curlHandle = curl_easy_init();
		struct curl_slist *headerList = nullptr;
		headerList = curl_slist_append(headerList, ("x-api-key: " + apiKey).c_str());
		headerList = curl_slist_append(headerList, "anthropic-version: 2023-06-01");
		headerList = curl_slist_append(headerList, "content-type: application/json");

		curl_easy_setopt(curlHandle, CURLOPT_URL, ("https://api.anthropic.com" + path).c_str());
		curl_easy_setopt(curlHandle, CURLOPT_HTTPHEADER, headerList);
		curl_easy_setopt(curlHandle, CURLOPT_POSTFIELDS, requestText.c_str());
		curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, writeCallback);
		curl_easy_setopt(curlHandle, CURLOPT_WRITEDATA, &responseText);

		CURLcode result = curl_easy_perform(curlHandle);

		long httpStatus = 0;
		curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &httpStatus);
		curl_slist_free_all(headerList);
		curl_easy_cleanup(curlHandle);

		if(result != CURLE_OK) {
			throw std::runtime_error(curl_easy_strerror(result));
		}
		Log("ai", "http %ld, %zu bytes received", httpStatus, responseText.size());
		return json::parse(responseText);
	}
};
