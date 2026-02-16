#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <inttypes.h>

#include "display_ui.h"
#include "logging_utils.h"
#include "wifi_utils.h"

namespace {
constexpr char kPrefsNamespace[] = "elcfg";
constexpr char kAreaKey[] = "np_area";
constexpr char kCurrencyKey[] = "np_curr";
constexpr char kDefaultNordpoolArea[] = "SE3";
constexpr char kDefaultNordpoolCurrency[] = "SEK";
constexpr const char *kNordpoolAreas[] = {
    "SE1", "SE2", "SE3", "SE4", "NO1", "NO2", "NO3", "NO4", "NO5",
    "DK1", "DK2", "FI",  "EE",  "LV",  "LT",  "SYS"};
constexpr const char *kNordpoolCurrencies[] = {"SEK", "EUR", "NOK", "DKK"};
constexpr size_t kNordpoolAreaCount = sizeof(kNordpoolAreas) / sizeof(kNordpoolAreas[0]);
constexpr size_t kNordpoolCurrencyCount = sizeof(kNordpoolCurrencies) / sizeof(kNordpoolCurrencies[0]);
constexpr size_t kAreaMaxLen = 8;
constexpr size_t kCurrencyMaxLen = 8;
constexpr uint32_t kReconnectCooldownMs = 5000;

bool gSaveConfigRequested = false;
uint32_t gLastReconnectAttemptMs = 0;

constexpr char kPortalCustomHead[] PROGMEM = R"HTML(
<script>
(function () {
  var areaOptions = ["SE1","SE2","SE3","SE4","NO1","NO2","NO3","NO4","NO5","DK1","DK2","FI","EE","LV","LT","SYS"];
  var currencyOptions = ["SEK","EUR","NOK","DKK"];

  function replaceInputWithSelect(inputId, options) {
    var input = document.getElementById(inputId);
    if (!input || input.tagName !== "INPUT") return;

    var selected = (input.value || "").toUpperCase();
    var select = document.createElement("select");
    select.id = input.id;
    select.name = input.name;
    select.style.width = "100%";

    var hasSelected = false;
    for (var i = 0; i < options.length; i++) {
      if (options[i] === selected) {
        hasSelected = true;
        break;
      }
    }
    if (!hasSelected && options.length > 0) {
      selected = options[0];
    }

    for (var j = 0; j < options.length; j++) {
      var option = document.createElement("option");
      option.value = options[j];
      option.text = options[j];
      select.appendChild(option);
    }

    select.value = selected;
    input.parentNode.replaceChild(select, input);
  }

  window.addEventListener("load", function () {
    replaceInputWithSelect("NordPoolArea", areaOptions);
    replaceInputWithSelect("NordPoolCurrency", currencyOptions);
  });
})();
</script>
)HTML";

bool isAllowedToken(const String &value, const char *const *allowedValues, size_t allowedCount) {
  for (size_t i = 0; i < allowedCount; ++i) {
    if (value == allowedValues[i]) {
      return true;
    }
  }
  return false;
}

String normalizeToken(
    String value,
    const char *fallback,
    size_t maxLen,
    const char *const *allowedValues,
    size_t allowedCount) {
  value.trim();
  value.toUpperCase();
  if (value.isEmpty()) {
    value = fallback;
  }
  if (value.length() > maxLen) {
    value = value.substring(0, maxLen);
  }
  if (!isAllowedToken(value, allowedValues, allowedCount)) {
    value = fallback;
  }
  return value;
}

void normalizeSecrets(AppSecrets &secrets) {
  secrets.nordpoolArea =
      normalizeToken(secrets.nordpoolArea, kDefaultNordpoolArea, kAreaMaxLen, kNordpoolAreas, kNordpoolAreaCount);
  secrets.nordpoolCurrency = normalizeToken(
      secrets.nordpoolCurrency,
      kDefaultNordpoolCurrency,
      kCurrencyMaxLen,
      kNordpoolCurrencies,
      kNordpoolCurrencyCount);
}

void saveConfigCallback() {
  gSaveConfigRequested = true;
}

bool waitForConnection(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

void saveSecretsToPrefs(const AppSecrets &secrets) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    logf("Secrets save failed: prefs begin");
    return;
  }
  prefs.putString(kAreaKey, secrets.nordpoolArea);
  prefs.putString(kCurrencyKey, secrets.nordpoolCurrency);
  prefs.end();
  logf("Secrets saved: area=%s currency=%s", secrets.nordpoolArea.c_str(), secrets.nordpoolCurrency.c_str());
}

}  // namespace

void loadAppSecrets(AppSecrets &out) {
  out.nordpoolArea = kDefaultNordpoolArea;
  out.nordpoolCurrency = kDefaultNordpoolCurrency;

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    out.nordpoolArea = prefs.getString(kAreaKey, out.nordpoolArea);
    out.nordpoolCurrency = prefs.getString(kCurrencyKey, out.nordpoolCurrency);
    prefs.end();
  }

  normalizeSecrets(out);
}

bool wifiConnectWithConfigPortal(AppSecrets &secrets, uint16_t portalTimeoutSeconds) {
  loadAppSecrets(secrets);
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  WiFi.mode(WIFI_STA);

  char areaBuffer[kAreaMaxLen + 1];
  char currencyBuffer[kCurrencyMaxLen + 1];
  secrets.nordpoolArea.toCharArray(areaBuffer, sizeof(areaBuffer));
  secrets.nordpoolCurrency.toCharArray(currencyBuffer, sizeof(currencyBuffer));

  WiFiManager wifiManager;
  WiFiManagerParameter areaParam("NordPoolArea", "Nord Pool area:", areaBuffer, sizeof(areaBuffer));
  WiFiManagerParameter currencyParam("NordPoolCurrency", "currency:", currencyBuffer, sizeof(currencyBuffer));

  gSaveConfigRequested = false;
  wifiManager.addParameter(&areaParam);
  wifiManager.addParameter(&currencyParam);
  wifiManager.setConfigPortalTimeout(portalTimeoutSeconds);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setCustomHeadElement(kPortalCustomHead);
  wifiManager.setDarkMode(true);

  char apName[32];
  snprintf(apName, sizeof(apName), "ElMeter-%" PRIx64, ESP.getEfuseMac());
  const String apNameString(apName);

  wifiManager.setAPCallback([apNameString, portalTimeoutSeconds](WiFiManager *) {
    displayDrawWifiConfigPortal(apNameString.c_str(), portalTimeoutSeconds);
  });
  wifiManager.setConfigPortalTimeoutCallback([portalTimeoutSeconds]() {
    displayDrawWifiConfigTimeout(portalTimeoutSeconds);
  });

  logf("WiFiManager autoConnect start: AP='%s' timeout=%us", apName, (unsigned)portalTimeoutSeconds);
  if (!wifiManager.autoConnect(apName)) {
    logf("WiFiManager failed or timed out");
    return false;
  }

  if (gSaveConfigRequested) {
    secrets.nordpoolArea = String(areaParam.getValue());
    secrets.nordpoolCurrency = String(currencyParam.getValue());
    normalizeSecrets(secrets);
    saveSecretsToPrefs(secrets);
    gSaveConfigRequested = false;
  } else {
    normalizeSecrets(secrets);
  }

  logf("WiFi connected: ssid='%s' ip=%s area=%s currency=%s",
       WiFi.SSID().c_str(),
       WiFi.localIP().toString().c_str(),
       secrets.nordpoolArea.c_str(),
       secrets.nordpoolCurrency.c_str());
  return true;
}

bool wifiReconnect(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  const uint32_t now = millis();
  if (now - gLastReconnectAttemptMs < kReconnectCooldownMs) {
    return false;
  }
  gLastReconnectAttemptMs = now;

  WiFi.mode(WIFI_STA);
  logf("WiFi reconnect start");
  WiFi.begin();

  if (waitForConnection(timeoutMs)) {
    logf("WiFi connected: ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }

  logf("WiFi reconnect timeout: status=%d", WiFi.status());
  return false;
}

void wifiResetSettings() {
  WiFiManager wifiManager;
  wifiManager.resetSettings();

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
}
