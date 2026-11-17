using System.Text.Json;
using NessStudio.Recording.Engines;

internal static class SelectionContracts
{
    internal static int Run()
    {
        var cases = new List<string>();
        void Check(string name, bool value)
        {
            if (!value) throw new InvalidDataException(name);
            cases.Add(name);
        }
        RecordingExecutionMode Selected(string metrics)
        {
            using var snapshot = JsonDocument.Parse("{\"metrics\":" + metrics + "}");
            return NativeRecordingSelection.EffectiveMode(snapshot.RootElement);
        }
        void Reject(string name, string metrics)
        {
            try { _ = Selected(metrics); }
            catch (InvalidDataException) { cases.Add(name); return; }
            throw new InvalidDataException(name);
        }
        const string gpu = "{\"encoderSelection\":{\"effectiveMode\":\"RequireGpuResidentHardware\"}}";
        const string cpu = "{\"encoderSelection\":{\"effectiveMode\":\"FullSoftware\"}}";
        try
        {
            Check("persisted-mode-values-preserved", (int)RecordingExecutionMode.FullSoftware == 2 && (int)RecordingExecutionMode.GpuWithSoftwareEncoding == 3);
            Check("hardware-primary-with-software-camera-is-hybrid",
                Selected(gpu[..^1] + ",\"cameraTrack\":" + cpu + "}") == RecordingExecutionMode.GpuWithSoftwareEncoding);
            Check("software-primary-with-hardware-program-is-hybrid",
                Selected(cpu[..^1] + ",\"programTrack\":" + gpu + "}") == RecordingExecutionMode.GpuWithSoftwareEncoding);
            Check("all-software-tracks-remain-full-software",
                Selected(cpu[..^1] + ",\"programTrack\":" + cpu + "}") == RecordingExecutionMode.FullSoftware);
            Check("explicit-readback-is-hybrid", Selected("{\"encoderSelection\":{\"effectiveMode\":\"GpuWithSoftwareEncoding\"}}") == RecordingExecutionMode.GpuWithSoftwareEncoding);
            Reject("unknown-native-mode-is-not-reported-as-gpu", "{\"encoderSelection\":{\"effectiveMode\":\"FutureMode\"}}");
            Reject("missing-primary-selection-is-rejected", "{}");
            Reject("missing-secondary-selection-is-rejected", gpu[..^1] + ",\"cameraTrack\":{}}");
            using var device = JsonDocument.Parse("{\"id\":\"usb-camera-a-directshow\",\"aliases\":[\"usb-camera-a-old-interface\"]}");
            Check("legacy-camera-alias-is-recognized", NativeRecordingSelection.CameraMatches(device.RootElement, "USB-CAMERA-A-OLD-INTERFACE"));
            Check("different-camera-instance-is-not-substituted", !NativeRecordingSelection.CameraMatches(device.RootElement, "usb-camera-b-old-interface"));
            Console.WriteLine(JsonSerializer.Serialize(new { status = "PASS", cases }));
            return 0;
        }
        catch (Exception error)
        {
            Console.WriteLine(JsonSerializer.Serialize(new { status = "FAIL", error = error.Message, passedCases = cases }));
            return 1;
        }
    }
}
