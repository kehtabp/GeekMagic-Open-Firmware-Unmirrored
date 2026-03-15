function displayHandler() {
  return {
    brightness: 100,
    weatherLocation: "",
    weatherApiKey: "",
    cryptoCoins: "",
    message: "",
    cryptoMessage: "",
    loading: false,

    init() {
      apiFetch("/api/v1/display/brightness")
        .then((r) => r.json())
        .then((data) => {
          this.brightness = data.brightness ?? 100;
        })
        .catch((err) => console.error("failed to fetch brightness", err));

      apiFetch("/api/v1/weather/config")
        .then((r) => r.json())
        .then((data) => {
          this.weatherLocation = data.weather_location || "";
          this.weatherApiKey = data.weather_api_key || "";
        })
        .catch((err) => console.error("failed to fetch weather config", err));

      apiFetch("/api/v1/crypto/config")
        .then((r) => r.json())
        .then((data) => {
          this.cryptoCoins = data.crypto_coins || "";
        })
        .catch((err) => console.error("failed to fetch crypto config", err));
    },

    setBrightness(value) {
      this.brightness = parseInt(value);
      clearTimeout(this._brightnessTimer);
      this._brightnessTimer = setTimeout(() => {
        apiFetch("/api/v1/display/brightness", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ brightness: this.brightness }),
        }).catch((err) => console.error("failed to set brightness", err));
      }, 150);
    },

    saveWeather() {
      this.loading = true;
      this.message = "";
      apiFetch("/api/v1/weather/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          weather_location: this.weatherLocation,
          weather_api_key: this.weatherApiKey,
        }),
      })
        .then((r) => r.json())
        .then((data) => {
          this.message = data.status === "ok" ? "Saved!" : (data.message || "Error");
        })
        .catch(() => {
          this.message = "Request failed";
        })
        .finally(() => {
          this.loading = false;
        });
    },

    saveCrypto() {
      this.loading = true;
      this.cryptoMessage = "";
      apiFetch("/api/v1/crypto/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ crypto_coins: this.cryptoCoins }),
      })
        .then((r) => r.json())
        .then((data) => {
          this.cryptoMessage =
            data.status === "ok" ? "Saved!" : data.message || "Error";
        })
        .catch(() => {
          this.cryptoMessage = "Request failed";
        })
        .finally(() => {
          this.loading = false;
        });
    },
  };
}
