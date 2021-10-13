#include "cpptoml/cpptoml.h"
#include <curl/curl.h>
#include <iostream>
#include <json/json.h>
#include <string>

std::string urlEncode(const std::string &val)
{
  std::string ret;
  CURL *curl = curl_easy_init();
  if (!curl)
    return {};
  char *output = curl_easy_escape(curl, val.c_str(), val.size());
  if (!output)
    return {};
  ret = output;
  curl_free(output);
  return ret;
}

static auto w(void *contents, size_t sz, size_t nmemb, void *userp) -> size_t
{
  auto &str = *static_cast<std::string *>(userp);
  str += std::string{static_cast<const char *>(contents), sz * nmemb};
  return sz * nmemb;
};

std::string getAccessToken(const std::string &clientId, const std::string &clientSecret, const std::string &refreshToken)
{

  const auto payload = [&]() {
    std::ostringstream ss;
    ss << "client_secret=" << urlEncode(clientSecret) << "&grant_type=refresh_token&refresh_token=" << urlEncode(refreshToken)
       << "&client_id=" << urlEncode(clientId);
    return ss.str();
  }();

  auto curl = curl_easy_init();
  if (!curl)
    return {};
  curl_easy_setopt(curl, CURLOPT_URL, "https://oauth2.googleapis.com/token");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &w);
  std::string out;
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&out);

  auto res = curl_easy_perform(curl);
  if (res != CURLE_OK)
    fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));

  curl_easy_cleanup(curl);

  Json::Value root;
  std::istringstream ss(out);
  ss >> root;

  return root["access_token"].asString();
}

int main()
{
  curl_global_init(CURL_GLOBAL_ALL);
  auto toml = cpptoml::parse_file("credentials.toml");
  auto refreshToken = toml->get_as<std::string>("refresh-token").value_or("");
  auto clientId = toml->get_as<std::string>("client-id").value_or("");
  auto clientSecret = toml->get_as<std::string>("client-secret").value_or("");
  auto apiKey = toml->get_as<std::string>("api-key").value_or("");
  auto accessToken = getAccessToken(clientId, clientSecret, refreshToken);
  curl_global_cleanup();
}
