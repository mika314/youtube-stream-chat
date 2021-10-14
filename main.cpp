#include "cpptoml/cpptoml.h"
#include "sdlpp/sdlpp.hpp"
#include <codecvt>
#include <curl/curl.h>
#include <iostream>
#include <json/json.h>
#include <locale>
#include <mutex>
#include <string>
#include <unordered_set>

static std::string urlEncode(const std::string &val)
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

static auto getAccessToken(const std::string &clientId, const std::string &clientSecret, const std::string &refreshToken) -> std::string
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

static auto getChatId(const std::string &apiKey, const std::string &accessToken) -> std::string
{
  auto curl = curl_easy_init();
  if (!curl)
    return {};

  auto url = [&apiKey]() {
    std::ostringstream ss;
    ss << "https://youtube.googleapis.com/youtube/v3/liveBroadcasts?"
          "part=snippet%2CcontentDetails%2Cstatus&broadcastStatus=active&key="
       << urlEncode(apiKey);
    return ss.str();
  }();

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

  struct curl_slist *list = NULL;

  auto authorization = [&accessToken]() {
    std::ostringstream ss;
    ss << "Authorization: Bearer " << urlEncode(accessToken);
    return ss.str();
  }();

  list = curl_slist_append(list, authorization.c_str());
  list = curl_slist_append(list, "Accept: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &w);
  std::string out;
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&out);

  auto res = curl_easy_perform(curl);
  if (res != CURLE_OK)
    fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));

  curl_slist_free_all(list);

  curl_easy_cleanup(curl);

  Json::Value root;
  std::istringstream ss(out);
  ss >> root;
  assert(!root["items"].empty());
  return root["items"][0]["snippet"]["liveChatId"].asString();
}

struct Msg
{
  std::string id;
  std::string name;
  std::string msg;
};

struct Msgs
{
  std::string nextPageToken;
  std::vector<Msg> msgs;
};

static auto chat(const std::string &apiKey, const std::string &accessToken, const std::string &chatId, const std::string &pageToken = "") -> Msgs
{
  auto curl = curl_easy_init();
  if (!curl)
    return {};

  auto url = [&apiKey, &chatId, &pageToken]() {
    std::ostringstream ss;
    ss << "https://youtube.googleapis.com/youtube/v3/liveChat/messages?liveChatId=" << urlEncode(chatId) << "&part=snippet%2CauthorDetails&"
       << (!pageToken.empty() ? ("pageToken=" + pageToken + "&") : std::string{}) << "key=" << urlEncode(apiKey);
    return ss.str();
  }();

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

  struct curl_slist *list = NULL;

  auto authorization = [&accessToken]() {
    std::ostringstream ss;
    ss << "Authorization: Bearer " << urlEncode(accessToken);
    return ss.str();
  }();

  list = curl_slist_append(list, authorization.c_str());
  list = curl_slist_append(list, "Accept: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &w);
  std::string out;
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&out);

  auto res = curl_easy_perform(curl);
  if (res != CURLE_OK)
    fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));

  curl_slist_free_all(list);

  curl_easy_cleanup(curl);

  Json::Value root;
  std::istringstream ss(out);
  ss >> root;

  Msgs ret;
  ret.nextPageToken = root["nextPageToken"].asString();
  auto items = root["items"];
  for (auto i = 0U; i < items.size(); ++i)
  {
    auto msg = items[i];
    ret.msgs.push_back({msg["id"].asString(), msg["authorDetails"]["displayName"].asString(), msg["snippet"]["displayMessage"].asString()});
  }
  return ret;
}

static size_t emptyReadCb(char * /*buffer*/, size_t /*size*/, size_t /*nitems*/, void * /*userdata*/)
{
  return 0;
}

static std::string getTtsToken(const std::string &azureKey)
{
  std::string ret;
  auto curl = curl_easy_init();
  if (!curl)
  {
    curl_global_cleanup();
    throw std::runtime_error("curl_easy_init error");
  }

  curl_easy_setopt(curl, CURLOPT_URL, "https://eastus.api.cognitive.microsoft.com/sts/v1.0/issuetoken");
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ret);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, w);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  struct curl_slist *chunk = NULL;
  chunk = curl_slist_append(chunk, ("Ocp-Apim-Subscription-Key: " + azureKey).c_str());
  chunk = curl_slist_append(chunk, "Expect:");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
  //  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, emptyReadCb);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
  auto res = curl_easy_perform(curl);
  if (res != CURLE_OK)
  {
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    throw std::runtime_error("curl_easy_perform() failed"); // curl_easy_strerror(res)
  }
  long codep;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &codep);
  if (codep != 200)
  {
    curl_easy_cleanup(curl);
    std::cerr << codep << ":" << ret << std::endl;
    throw std::runtime_error("query token error");
  }

  curl_easy_cleanup(curl);
  curl_slist_free_all(chunk);
  return ret;
}

constexpr auto PauseSz = 2000;
static std::mutex mutex;

struct Ctx
{
  static constexpr float TalkThreshold = -12;
  Ctx(std::string azureKey)
    : azureKey(std::move(azureKey)),
      want([]() {
        SDL_AudioSpec want;
        want.freq = 24000;
        want.format = AUDIO_S16;
        want.channels = 1;
        want.samples = 4096;
        return want;
      }()),
      // Headset (USB-C to 3.5mm Headphone Jack Adapter)
      // Acer KG241 P (NVIDIA High Definition Audio)
      audio(nullptr,
            false,
            &want,
            &have,
            0,
            [this](Uint8 *stream, int len) {
              std::lock_guard<std::mutex> guard(mutex);
              int16_t *s = (int16_t *)stream;
              if (talking > 0 && ttsPaused())
              {
                for (auto i = 0u; i < len / sizeof(int16_t); ++i, ++s)
                  *s = 0;
              }
              else
              {
                for (auto i = 0u; i < len / sizeof(int16_t); ++i, ++s)
                  *s = idx < pcm.size() ? pcm[idx++] : 0;
              }
              return len;
            }),
      capture(nullptr, true, &want, &captureHave, 0, [this](Uint8 *stream, int len) {
        std::lock_guard<std::mutex> guard(mutex);
        auto pcm = reinterpret_cast<int16_t *>(stream);
        auto m = std::max_element(pcm, pcm + len / sizeof(int16_t));
        const auto db = 20 * logf(1.f * *m / 0x8000) / logf(10);
        if (db >= TalkThreshold)
          talking = 5;
        else if (talking > 0)
          --talking;
      })
  {
    const int count = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < count; ++i)
      std::clog << "Audio device" << i << " " << SDL_GetAudioDeviceName(i, 0) << "\n";
    audio.pause(false);
    capture.pause(false);
    tts("tts", "is running", true);
    // for (int i = 31; i <= 40; ++i)
    //   tts(std::to_string(i) + "_voice", "sample voice", true);
  }

  bool ttsPaused() const
  {
    if (pcm.size() - idx < PauseSz)
      return false;
    auto m = std::max_element(std::begin(pcm) + idx, std::begin(pcm) + idx + PauseSz);
    const auto db = 20 * logf(1.f * *m / 0x8000) / logf(10);
    return db < TalkThreshold;
  }

  auto tts(const std::string &name, const std::string &text, bool isMe) -> void;

  std::string azureKey;
  SDL_AudioSpec want;
  SDL_AudioSpec have;
  SDL_AudioSpec captureHave;
  sdl::Audio audio;
  sdl::Audio capture;
  std::string ttsToken;
  std::vector<int16_t> pcm;
  size_t idx = 0;
  std::string twitchCh;
  int talking = 0;
};

static bool isRu(const std::string &text)
{
  static std::unordered_set<char16_t> ruChars = {
    u'\x0410', u'\x0410', u'\x0411', u'\x0412', u'\x0413', u'\x0414', u'\x0415', u'\x0416', u'\x0417', u'\x0418', u'\x0419', u'\x041A', u'\x041B',
    u'\x041C', u'\x041D', u'\x041E', u'\x041F', u'\x0420', u'\x0421', u'\x0422', u'\x0423', u'\x0424', u'\x0425', u'\x0426', u'\x0427', u'\x0428',
    u'\x0429', u'\x042A', u'\x042B', u'\x042C', u'\x042D', u'\x042E', u'\x042F', u'\x0430', u'\x0431', u'\x0432', u'\x0433', u'\x0434', u'\x0435',
    u'\x0436', u'\x0437', u'\x0438', u'\x0439', u'\x043A', u'\x043B', u'\x043C', u'\x043D', u'\x043E', u'\x043F', u'\x0440', u'\x0441', u'\x0442',
    u'\x0443', u'\x0444', u'\x0445', u'\x0446', u'\x0447', u'\x0448', u'\x0449', u'\x044A', u'\x044B', u'\x044C', u'\x044D', u'\x044E', u'\x044F'};

  std::u16string utf16 = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(text.data());

  return std::count_if(std::begin(utf16), std::end(utf16), [](char16_t ch) { return ruChars.find(ch) != std::end(ruChars); }) > 0;
}

static std::unordered_map<std::string, std::string> loadVoices()
{
  std::unordered_map<std::string, std::string> ret;
  std::ifstream f("voices.txt");
  if (!f)
    std::cerr << "File voices.txt is missing\n";
  std::string line;
  while (std::getline(f, line))
  {
    std::istringstream strm(line);
    std::string name;
    std::string voice;
    strm >> name >> voice;
    ret[name] = voice;
  }
  return ret;
}

static std::string getVoice(const std::string &name, const std::string &text)
{
  if (!isRu(text))
  {
    static std::unordered_map<std::string, std::string> voicesMap = loadVoices();
    static std::array<std::string, 15> voices = {
      "en-CA-Linda",
      "en-AU-HayleyRUS",
      "en-AU-Catherine",
      "en-CA-HeatherRUS",
      "en-CA-Linda",
      "en-GB-HazelRUS",
      "en-AU-HayleyRUS",
      "en-GB-HazelRUS",
      "en-US-AriaRUS",
      "en-US-AriaRUS",
      "en-GB-George",
      "en-US-ZiraRUS",
      "en-US-AriaRUS",
      "en-US-BenjaminRUS",
      "en-US-Guy24kRUS",
    };

    //  en-AU-NatashaNeural
    //  en-CA-ClaraNeural
    //  en-GB-LibbyNeural
    //  en-GB-MiaNeural
    //  en-US-AriaNeural
    //  en-US-GuyNeural

    auto iter = voicesMap.find(name);
    if (iter != std::end(voicesMap))
      return iter->second;

    return voices[(std::hash<std::string>()(name) ^ 1) % voices.size()];
  }
  else
  {
    static std::array<std::string, 15> voices = {
      "ru-RU-DariyaNeural",
      "ru-RU-EkaterinaRUS",
      "ru-RU-Irina",
      "ru-RU-DariyaNeural",
      "ru-RU-EkaterinaRUS",
      "ru-RU-Irina",
      "ru-RU-DariyaNeural",
      "ru-RU-DariyaNeural",
      "ru-RU-EkaterinaRUS",
      "ru-RU-EkaterinaRUS",
      "ru-RU-Pavel",
      "ru-RU-EkaterinaRUS",
      "ru-RU-DariyaNeural",
      "ru-RU-Pavel",
      "ru-RU-Pavel",
    };

    return voices[(std::hash<std::string>()(name) ^ 1) % voices.size()];
  }
}

static std::string escName(std::string value)
{
  std::transform(std::begin(value), std::end(value), std::begin(value), [](char ch) {
    if (ch == '_')
      return ' ';
    return ch;
  });
  while (!value.empty() && isdigit(value.back()))
    value.resize(value.size() - 1);
  if (value == "cmaennche")
    value = "c-man-uh-she";
  if (value == "retr0m")
    value = "retro-m";
  if (value == "theemperorpalpatine")
    value = "Emperor Palpa-teen";
  if (value == "c0rzi")
    value = "corzi";
  return value;
}

static std::string getDialogLine(const std::string &text, bool isMe)
{
  if (isMe)
    return "";
  if (text.find("?") != std::string::npos || text.find("!") == 0)
    return "asked:";
  if (text.find("!") != std::string::npos)
    return "yelled:";
  return "said:";
}

static auto eq(const std::vector<std::string> &words, size_t i, size_t j, size_t w)
{
  if (i + w > words.size())
    return false;
  if (j + w > words.size())
    return false;
  for (auto k = 0U; k < w; ++k)
    if (words[i + k] != words[j + k])
      return false;
  return true;
}

static auto dedup(const std::string &var)
{
  std::vector<std::string> words;
  std::string word;
  std::istringstream st(var);
  while (std::getline(st, word, ' '))
    words.push_back(word);
  for (bool didUpdate = true; didUpdate;)
  {
    didUpdate = false;
    for (auto w = 1U; w < words.size() / 2 && !didUpdate; ++w)
      for (auto i = 0U; i < words.size() - w && !didUpdate; ++i)
        for (auto r = 1U; !didUpdate; ++r)
          if (!eq(words, i, i + r * w, w))
          {
            if (r >= 3)
            {
              words.erase(std::begin(words) + i + w, std::begin(words) + i + r * w);
              didUpdate = true;
            }
            else
              break;
          }
  }
  std::string ret;
  for (const auto &word : words)
  {
    if (!ret.empty())
      ret += " ";
    ret += word;
  }
  return ret;
}

static std::string escape(const std::string &name, std::string data)
{
  if (name == "tanja_ultramono")
    return "";

  // escape name
  size_t p0 = 0;
  while ((p0 = data.find('@', p0)) != std::string::npos)
  {
    ++p0;
    auto p1 = data.find(' ', p0);
    data.replace(p0, p1 - p0, escName(data.substr(p0, p1 - p0)));
  }

  // escape HTML links
  for (;;)
  {
    {
      auto p0 = data.find("http://");
      if (p0 != std::string::npos)
      {
        auto p1 = data.find(' ', p0);
        data.replace(p0, p1 - p0, "http link");

        continue;
      }
    }
    {
      auto p0 = data.find("https://");
      if (p0 != std::string::npos)
      {
        auto p1 = data.find(' ', p0);
        data.replace(p0, p1 - p0, "https link");

        continue;
      }
    }
    break;
  }

  // escape for XML
  std::string buffer;
  buffer.reserve(data.size());
  for (size_t pos = 0; pos != data.size(); ++pos)
  {
    switch (data[pos])
    {
    case '&': buffer.append("&amp;"); break;
    case '\"': buffer.append("&quot;"); break;
    case '\'': buffer.append("&apos;"); break;
    case '<': buffer.append("&lt;"); break;
    case '>': buffer.append("&gt;"); break;
    default: buffer.append(&data[pos], 1); break;
    }
  }
  return dedup(buffer);
}

static size_t readTextToPcmCb(char *buffer, size_t size, size_t nitems, void *ctx)
{
  std::string &inStr = *(std::string *)ctx;
  auto ret = std::min(inStr.size(), size * nitems);
  std::copy(std::begin(inStr), std::begin(inStr) + ret, buffer);
  inStr.erase(0, ret);
  return ret;
}

enum class NeedReauth {};

static std::vector<int16_t> textToPcm(const std::string &token, const std::string &name, const std::string &text, bool isMe)
{
  std::string pcmStr;
  auto curl = curl_easy_init();
  if (!curl)
  {
    curl_global_cleanup();
    throw std::runtime_error("curl_easy_init error");
  }

  curl_easy_setopt(curl, CURLOPT_URL, "https://eastus.tts.speech.microsoft.com/cognitiveservices/v1");
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &pcmStr);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, w);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  struct curl_slist *chunk = NULL;
  chunk = curl_slist_append(chunk, "Accept:");
  chunk = curl_slist_append(chunk, "User-Agent: curl/7.68.0");
  chunk = curl_slist_append(chunk, ("Authorization: Bearer " + token).c_str());
  chunk = curl_slist_append(chunk, "Content-Type: application/ssml+xml");
  chunk = curl_slist_append(chunk, "X-Microsoft-OutputFormat: raw-24khz-16bit-mono-pcm");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

  const auto voice = getVoice(name, text);
  std::cout << "voice: " << voice << std::endl;

  static std::string lastName;
  auto supressName = (lastName == name) && !isMe;

  std::string xml = R"(<speak version="1.0" xml:lang="en-us"><voice xml:lang="en-US" name=")" + voice + R"(">)" +
                    (!supressName ? (escName(name) + " " + getDialogLine(text, isMe) + " ") : "") + escape(name, text) + R"(</voice></speak>)";

  curl_easy_setopt(curl, CURLOPT_READDATA, &xml);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, readTextToPcmCb);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(xml.size()));
  auto res = curl_easy_perform(curl);
  if (res != CURLE_OK)
  {
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    throw std::runtime_error("curl_easy_perform() failed"); // curl_easy_strerror(res)
  }
  long codep;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &codep);
  if (codep == 401)
    throw NeedReauth{};
  if (codep != 200)
  {
    std::cerr << "http code: " << codep << std::endl;
    std::cerr << "content: " << pcmStr << std::endl;
    curl_easy_cleanup(curl);
    throw std::runtime_error("tts error");
  }
  lastName = name;

  curl_easy_cleanup(curl);
  curl_slist_free_all(chunk);

  std::vector<int16_t> ret;
  ret.resize(pcmStr.size() / sizeof(int16_t) + 2 * PauseSz);
  for (auto i = 0u; i < 2 * PauseSz; ++i)
    ret[i] = 0;
  memcpy(ret.data() + 2 * PauseSz, pcmStr.data(), pcmStr.size());
  std::cout << "pcm: " << ret.size() << std::endl;
  return ret;
}

auto Ctx::tts(const std::string &name, const std::string &text, bool isMe) -> void
{
  std::cout << name << ": " << text << std::endl;

  for (int i = 0; i < 2; ++i)
  {
    bool needAuth = false;
    try
    {
      auto tmpPcm = textToPcm(ttsToken, name, text, isMe);
      std::lock_guard<std::mutex> guard(mutex);
      if (idx >= pcm.size())
      {
        pcm = std::move(tmpPcm);
        idx = 0;
      }
      else
      {
        pcm.erase(std::begin(pcm), std::begin(pcm) + idx);
        idx = 0;
        if (pcm.size() > 30 * 24000)
        {
          pcm.resize(std::max(tmpPcm.size(), pcm.size()));
          for (auto i = 0u; i < tmpPcm.size(); ++i)
            pcm[i + idx] = std::min(32000, std::max(-32000, pcm[i + idx] + tmpPcm[i]));
        }
        else
        {
          for (auto i = 0u; i < tmpPcm.size(); ++i)
            pcm.push_back(tmpPcm[i]);
        }
      }
    }
    catch (NeedReauth)
    {
      needAuth = true;
      std::clog << "401 we need to re-authenticate on Azure TTS\n";
    }
    catch (std::exception &e)
    {
      std::cerr << e.what() << std::endl;
    }
    if (!needAuth)
      return;
    ttsToken = getTtsToken(azureKey);
  }
  std::cerr << "giving up to TTS\n";
}

int main()
{
  curl_global_init(CURL_GLOBAL_ALL);
  sdl::Init sdl(SDL_INIT_EVERYTHING);

  const auto toml = cpptoml::parse_file("credentials.toml");
  const auto refreshToken = toml->get_as<std::string>("refresh-token").value_or("");
  const auto clientId = toml->get_as<std::string>("client-id").value_or("");
  const auto clientSecret = toml->get_as<std::string>("client-secret").value_or("");
  const auto apiKey = toml->get_as<std::string>("api-key").value_or("");
  const auto azureKey = toml->get_as<std::string>("azure-key").value_or("");
  const auto accessToken = getAccessToken(clientId, clientSecret, refreshToken);
  const auto chatId = getChatId(apiKey, accessToken);

  Ctx ctx(azureKey);

  auto token = std::string{};
  std::unordered_set<std::string> ids;
  bool first = true;
  for (;;)
  {
    auto msgs = chat(apiKey, accessToken, chatId, token);
    token = msgs.nextPageToken;
    for (const auto &msg : msgs.msgs)
    {
      if (ids.find(msg.id) != std::end(ids))
        continue;
      std::cout << msg.name << ": " << msg.msg << std::endl;
      if (!first)
        ctx.tts(msg.name, msg.msg, false);
      ids.insert(msg.id);
    }
    sleep(6);
    first = false;
  }

  curl_global_cleanup();
}
