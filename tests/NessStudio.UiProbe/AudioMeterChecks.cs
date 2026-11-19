using System.Buffers.Binary;
using System.Reflection;
using NAudio.Wave;
using NessStudio.Presentation;

internal static partial class Program
{
    private static async Task AudioMeterContractsAsync()
    {
        var measure = typeof(AudioLevelMonitor).GetMethod("MeasurePeak", BindingFlags.NonPublic | BindingFlags.Static)!;
        double Peak(byte[] bytes, WaveFormat format, int? count = null) =>
            (double)measure.Invoke(null, [bytes, count ?? bytes.Length, format])!;
        static byte[] FloatSamples(params float[] samples)
        {
            var bytes = new byte[samples.Length * sizeof(float)];
            for (int index = 0; index < samples.Length; index++)
                BinaryPrimitives.WriteSingleLittleEndian(bytes.AsSpan(index * sizeof(float)), samples[index]);
            return bytes;
        }

        var floatFormat = WaveFormat.CreateIeeeFloatWaveFormat(48000, 2);
        Check(Peak(FloatSamples(0, -.65f, .2f), floatFormat) is > .649 and < .651,
            "Floating-point meter missed a negative peak or another channel.");
        Check(Peak(FloatSamples(0, float.NaN, float.PositiveInfinity, -.5f), floatFormat) == .5,
            "Non-finite input corrupted the live audio meter.");
        Check(Peak(FloatSamples(-2, 4), floatFormat) == 1,
            "Clipped input escaped the meter's 0..100 range.");
        Check(Peak(FloatSamples(0, .25f, 1), floatFormat, 8) == .25,
            "Meter read samples beyond BytesRecorded.");
        Check(Peak([0, 128, 255], new WaveFormat(48000, 8, 1)) == 1,
            "Unsigned PCM8 was not centered at 128.");
        Check(Peak([0, 0, 0, 192, 0], new WaveFormat(48000, 16, 1)) == .5,
            "PCM16 peak or incomplete final sample was decoded incorrectly.");
        Check(Peak([0, 0, 0, 0, 0, 192], new WaveFormat(48000, 24, 1)) == .5,
            "PCM24 negative samples were not sign-extended.");
        Check(Peak([0, 0, 0, 192], new WaveFormat(48000, 32, 1)) == .5,
            "PCM32 negative samples were decoded incorrectly.");
        var doubleFormat = WaveFormat.CreateCustomFormat(WaveFormatEncoding.IeeeFloat, 48000, 1, 48000 * 8, 8, 64);
        var doubleBytes = new byte[8];
        BinaryPrimitives.WriteDoubleLittleEndian(doubleBytes, -.75);
        Check(Peak(doubleBytes, doubleFormat) == .75, "Float64 meter input was decoded incorrectly.");
        Check(Peak([], floatFormat) == 0 && Peak(FloatSamples(0, 0), floatFormat) == 0,
            "Empty input or digital silence produced a signal.");
        Results.Add(new { name = "audio-meter-raw-multichannel-pcm-float-peaks-clipping-and-buffer-bounds" });

        // Verify a brief input peak survives until the next presentation tick and that
        // a render device delivering no more packets eventually displays silence.
        var signalType = typeof(AudioLevelMonitor).GetNestedType("Signal", BindingFlags.NonPublic)!;
        object signal = Activator.CreateInstance(signalType, nonPublic: true)!;
        var update = signalType.GetMethod("Update", BindingFlags.NonPublic | BindingFlags.Instance)!;
        var read = signalType.GetMethod("Read", BindingFlags.NonPublic | BindingFlags.Instance)!;
        var reset = signalType.GetMethod("Reset", BindingFlags.NonPublic | BindingFlags.Instance)!;
        double Read() => (double)read.Invoke(signal, null)!;
        update.Invoke(signal, [.8]);
        update.Invoke(signal, [.1]);
        Check(Read() == 80 && Read() == 10, "A short audio peak was lost between presentation ticks.");
        await Task.Delay(300);
        Check(Read() == 0, "A loopback meter retained a stale peak after playback stopped.");
        update.Invoke(signal, [.7]);
        reset.Invoke(signal, null);
        Check(Read() == 0, "Disabling or replacing an audio source retained its previous signal.");

        var monitor = new AudioLevelMonitor();
        monitor.Configure(null, null);
        Check(monitor.ReadLevels() == (0d, 0d), "Disabled audio sources showed activity.");
        await monitor.DisposeAsync();
        monitor.Configure("disposed-microphone", "disposed-loopback");
        Check(monitor.ReadLevels() == (0d, 0d), "A disposed monitor accepted new endpoint capture.");
        await monitor.DisposeAsync();
        Results.Add(new { name = "audio-meter-transient-hold-loopback-silence-reset-and-idempotent-disposal" });
    }
}
