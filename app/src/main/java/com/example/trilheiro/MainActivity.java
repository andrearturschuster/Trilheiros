package com.example.trilheiro;

import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import com.google.androidgamesdk.GameActivity;

public class MainActivity extends GameActivity implements SensorEventListener {
    static {
        System.loadLibrary("trilheiro");
    }

    private SensorManager sensorManager;
    private Sensor accelerometer;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sensorManager = (SensorManager) getSystemService(SENSOR_SERVICE);
        accelerometer = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (accelerometer != null) {
            sensorManager.registerListener(this, accelerometer, SensorManager.SENSOR_DELAY_GAME);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        sensorManager.unregisterListener(this);
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {
            // Send tilt data to native code
            setTilt(event.values[1]); // Y axis usually handles tilt in landscape
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    public native void setTilt(float tilt);
    public native void setDamageEnabled(boolean enabled);
    public native void resetVehicle();

    public void savePlayerData(long money, String vehicleData, String addonData, float fuel, float fuelCap) {
        android.content.SharedPreferences prefs = getSharedPreferences("TrilheiroPrefs", MODE_PRIVATE);
        android.content.SharedPreferences.Editor editor = prefs.edit();
        editor.putLong("money", money);
        editor.putString("vehicles", vehicleData);
        editor.putString("addons", addonData);
        editor.putFloat("fuel", fuel);
        editor.putFloat("fuelCap", fuelCap);
        editor.apply();
    }

    public String loadPlayerData() {
        android.content.SharedPreferences prefs = getSharedPreferences("TrilheiroPrefs", MODE_PRIVATE);
        long money = prefs.getLong("money", 1000);
        String vehicles = prefs.getString("vehicles", "");
        String addons = prefs.getString("addons", "");
        float fuel = prefs.getFloat("fuel", 100.0f);
        float fuelCap = prefs.getFloat("fuelCap", 100.0f);
        return money + "|" + vehicles + "|" + addons + "|" + fuel + "|" + fuelCap;
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);

        if (hasFocus) {
            hideSystemUi();
        }
    }

    private void hideSystemUi() {
        WindowInsetsController insetsController = getWindow().getInsetsController();
        if (insetsController != null) {
            insetsController.hide(WindowInsets.Type.systemBars());
            insetsController.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }
}