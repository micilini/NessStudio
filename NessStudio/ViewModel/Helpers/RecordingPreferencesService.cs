using NessStudio.Models;
using System;
using System.IO;
using System.Text.Json;
using System.Windows;
namespace NessStudio.ViewModel.Helpers
{
    public class RecordingPreferencesService
    {
        private static readonly JsonSerializerOptions JsonOptions = new JsonSerializerOptions
        {
            WriteIndented = true
        };
        public string GetPreferencesFilePath()
        {
            var app = (App)Application.Current;
            return Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "NessStudio",
                app.RecordingPreferencesFileName
            );
        }
        public void EnsurePreferencesFileExists()
        {
            var path = GetPreferencesFilePath();
            if (File.Exists(path))
                return;
            var model = CreateDefaultModel();
            Save(model);
            DebugLog.Write("[REC-PREFS] file created with defaults");
        }
        public RecordingPreferencesModel Load()
        {
            var path = GetPreferencesFilePath();
            EnsurePreferencesFileExists();
            try
            {
                var json = File.ReadAllText(path);
                var model = JsonSerializer.Deserialize<RecordingPreferencesModel>(json, JsonOptions);
                if (model == null)
                    throw new InvalidOperationException("Preferences JSON returned null model.");
                model.TimerSeconds = NormalizeTimer(model.TimerSeconds);
                model.RecordingFps = NormalizeFps(model.RecordingFps);
                if (model.CreatedAtUtc == default)
                    model.CreatedAtUtc = DateTime.UtcNow;
                if (model.UpdatedAtUtc == default)
                    model.UpdatedAtUtc = DateTime.UtcNow;
                DebugLog.Write($"[REC-PREFS] file loaded | timer={model.TimerSeconds} | fps={model.RecordingFps}");
                return model;
            }
            catch (Exception ex)
            {
                var fallback = CreateDefaultModel();
                Save(fallback);
                DebugLog.Write("[REC-PREFS] file corrupted, recreated with defaults:\n" + ex);
                return fallback;
            }
        }
        public void Save(RecordingPreferencesModel model)
        {
            if (model == null)
                throw new ArgumentNullException(nameof(model));
            var path = GetPreferencesFilePath();
            var directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrWhiteSpace(directory))
                Directory.CreateDirectory(directory);
            model.TimerSeconds = NormalizeTimer(model.TimerSeconds);
            model.RecordingFps = NormalizeFps(model.RecordingFps);
            var now = DateTime.UtcNow;
            if (model.CreatedAtUtc == default)
                model.CreatedAtUtc = now;
            model.UpdatedAtUtc = now;
            var json = JsonSerializer.Serialize(model, JsonOptions);
            var tempPath = path + ".tmp";
            File.WriteAllText(tempPath, json);
            if (File.Exists(path))
                File.Delete(path);
            File.Move(tempPath, path);
        }
        public RecordingPreferencesModel ResetToDefault()
        {
            var model = CreateDefaultModel();
            Save(model);
            DebugLog.Write("[REC-PREFS] file reset to defaults");
            return model;
        }
        public static int NormalizeTimer(int value)
        {
            return value switch
            {
                0 => 0,
                3 => 3,
                5 => 5,
                10 => 10,
                60 => 60,
                _ => 3
            };
        }
        public static int NormalizeFps(int value)
        {
            return value switch
            {
                24 => 24,
                25 => 25,
                30 => 30,
                48 => 48,
                50 => 50,
                60 => 60,
                _ => 30
            };
        }
        private static RecordingPreferencesModel CreateDefaultModel()
        {
            var now = DateTime.UtcNow;
            return new RecordingPreferencesModel
            {
                TimerSeconds = 3,
                RecordingFps = 30,
                CreatedAtUtc = now,
                UpdatedAtUtc = now
            };
        }
    }
}