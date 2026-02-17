#include "nordpool_ma_store.h"

#include <FS.h>
#include <SPIFFS.h>

#include "logging_utils.h"

namespace {
constexpr char kMovingAveragePath[] = "/nordpool_ma.bin";

bool ensureSpiffsMounted() {
  static bool attempted = false;
  static bool mounted = false;
  if (!attempted) {
    attempted = true;
    mounted = SPIFFS.begin(true);
    logf("SPIFFS mount: %s", mounted ? "ok" : "failed");
    if (mounted) {
      logf("SPIFFS info: used=%u total=%u", (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
    }
  }
  return mounted;
}
}  // namespace

void resetMovingAverageStore(MovingAverageStore &store) {
  store = MovingAverageStore();
}

bool loadMovingAverageStore(MovingAverageStore &store) {
  if (!ensureSpiffsMounted()) return false;

  File file = SPIFFS.open(kMovingAveragePath, FILE_READ);
  if (!file) return false;

  if ((size_t)file.size() != sizeof(MovingAverageStore)) {
    file.close();
    return false;
  }

  const size_t readBytes = file.read((uint8_t *)&store, sizeof(MovingAverageStore));
  file.close();
  if (readBytes != sizeof(MovingAverageStore)) return false;
  if (store.magic != kMovingAverageStoreMagic || store.version != kMovingAverageStoreVersion) return false;
  if (store.windowSamples == 0 || store.windowSamples > kMaxMovingAverageWindowSamples) return false;
  if (store.head >= store.windowSamples) return false;
  if (store.count > store.windowSamples) return false;
  return true;
}

bool saveMovingAverageStore(const MovingAverageStore &store) {
  if (!ensureSpiffsMounted()) return false;

  File file = SPIFFS.open(kMovingAveragePath, FILE_WRITE);
  if (!file) return false;

  const size_t written = file.write((const uint8_t *)&store, sizeof(MovingAverageStore));
  file.flush();
  file.close();
  return written == sizeof(MovingAverageStore);
}

void addMovingAverageSample(MovingAverageStore &store, float value) {
  if (store.windowSamples == 0 || store.windowSamples > kMaxMovingAverageWindowSamples) {
    store.windowSamples = kMovingAverageWindowHours;
  }

  store.values[store.head] = value;
  store.head = (store.head + 1) % store.windowSamples;
  if (store.count < store.windowSamples) ++store.count;
}

float movingAverageValue(const MovingAverageStore &store) {
  if (store.count == 0) return 0.0f;

  float sum = 0.0f;
  for (size_t i = 0; i < store.count; ++i) {
    sum += store.values[i];
  }
  return sum / (float)store.count;
}

