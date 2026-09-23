// Host-side unit test for playlist JSON parsing helpers (mirrors dashboard.ino).
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cassert>

#define MAX_STATIONS 40
#define NAME_LEN 28
#define URL_LEN 512

struct RadioStation {
  char name[NAME_LEN];
  char url[URL_LEN];
};

static RadioStation playlist[MAX_STATIONS];
static int totalStations = 0;

static void addStation(const char* name, const char* url) {
  if (totalStations >= MAX_STATIONS || !url || !url[0]) return;
  strncpy(playlist[totalStations].name, name, NAME_LEN - 1);
  playlist[totalStations].name[NAME_LEN - 1] = '\0';
  strncpy(playlist[totalStations].url, url, URL_LEN - 1);
  playlist[totalStations].url[URL_LEN - 1] = '\0';
  totalStations++;
}

static bool extractJsonFieldBounded(const char* start, const char* end, const char* field,
                                    char* dest, size_t destLen) {
  if (!start || !end || start >= end) return false;
  char key[24];
  snprintf(key, sizeof(key), "\"%s\"", field);
  size_t span = (size_t)(end - start);
  const char* pos = nullptr;
  for (size_t i = 0; i + strlen(key) <= span; i++) {
    if (strncmp(start + i, key, strlen(key)) == 0) {
      pos = start + i;
      break;
    }
  }
  if (!pos) return false;
  pos += strlen(key);
  while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == ':')) pos++;
  if (pos >= end || *pos != '"') return false;
  pos++;
  size_t i = 0;
  while (pos < end && *pos && *pos != '"' && i < destLen - 1) {
    if (*pos == '\\' && (pos + 1) < end && pos[1]) pos++;
    dest[i++] = *pos++;
  }
  dest[i] = '\0';
  return i > 0;
}

static bool parsePlaylistJson(const char* json) {
  if (!json || !json[0]) return false;
  totalStations = 0;
  const char* cursor = json;
  while (totalStations < MAX_STATIONS) {
    const char* objStart = strchr(cursor, '{');
    if (!objStart) break;
    const char* objEnd = strchr(objStart, '}');
    if (!objEnd) break;
    objEnd++;
    char name[NAME_LEN], url[URL_LEN];
    name[0] = url[0] = '\0';
    if (extractJsonFieldBounded(objStart, objEnd, "name", name, sizeof(name)) &&
        extractJsonFieldBounded(objStart, objEnd, "url", url, sizeof(url)) && url[0]) {
      addStation(name, url);
    }
    cursor = objEnd;
  }
  return totalStations > 0;
}

static int cycleSleep(int sleepMinutes) {
  static const int options[] = {0, 15, 30, 60};
  for (int i = 0; i < 4; i++) {
    if (options[i] == sleepMinutes) return options[(i + 1) % 4];
  }
  return 0;
}

static int batteryPercentFromVoltage(float v) {
  int p = (int)((v - 3.3f) * 100.0f / (4.2f - 3.3f));
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return p;
}

int main() {
  const char* json =
    "["
    "{\"name\":\"Alpha Radio\",\"url\":\"http://a.example/stream\"},"
    "{\"name\":\"Beta FM\",\"url\":\"http://b.example/live\"},"
    "{\"name\":\"No URL\"},"
    "{\"name\":\"Gamma\",\"url\":\"http://c.example/x\"}"
    "]";
  assert(parsePlaylistJson(json));
  assert(totalStations == 3);
  assert(strcmp(playlist[0].name, "Alpha Radio") == 0);
  assert(strcmp(playlist[1].url, "http://b.example/live") == 0);
  assert(strcmp(playlist[2].name, "Gamma") == 0);
  assert(!parsePlaylistJson(""));  // early-return leaves prior list intact
  assert(totalStations == 3);
  totalStations = 0;
  assert(!parsePlaylistJson(""));
  assert(totalStations == 0);

  assert(cycleSleep(0) == 15);
  assert(cycleSleep(15) == 30);
  assert(cycleSleep(30) == 60);
  assert(cycleSleep(60) == 0);

  assert(batteryPercentFromVoltage(3.3f) == 0);
  assert(batteryPercentFromVoltage(4.2f) == 100);
  assert(batteryPercentFromVoltage(3.75f) == 50);

  printf("PASS playlist=%d sleep-cycle battery-map\n", 3);
  return 0;
}
