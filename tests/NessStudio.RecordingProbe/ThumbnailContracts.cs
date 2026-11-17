using System.Security.Cryptography;
using System.Text.Json;
using NessStudio.Interop.Native;
using NessStudio.Recording;

internal static class ThumbnailContracts
{
    public static async Task<int> RunAsync(string package, string normal, string shortClip, string output)
    {
        var cases = new List<object>();
        try
        {
            if (Directory.Exists(output)) throw new IOException("Use a fresh evidence directory.");
            Directory.CreateDirectory(output);
            var service = new NativeThumbnailService(package);
            foreach (var (name, input) in new[] { ("normal", normal), ("short", shortClip) })
            {
                string path = Path.Combine(output, name + "-ç-東.png");
                JsonElement receipt = await service.CreateAsync(input, path, TimeSpan.FromSeconds(2));
                cases.Add(new { name, status = "PASS", path, receipt });
            }
            string existing = Path.Combine(output, "normal-ç-東.png");
            byte[] original = SHA256.HashData(await File.ReadAllBytesAsync(existing));
            try
            {
                await service.CreateAsync(normal, existing, TimeSpan.Zero);
                throw new InvalidDataException("An existing image was silently replaced.");
            }
            catch (NativeEngineException)
            {
                byte[] preserved = SHA256.HashData(await File.ReadAllBytesAsync(existing));
                if (!original.SequenceEqual(preserved))
                    throw new InvalidDataException("A rejected operation changed the image.");
                cases.Add(new { name = "preserve-existing", status = "PASS" });
            }
            string missingOutput = Path.Combine(output, "missing.png");
            try
            {
                await service.CreateAsync(Path.Combine(output, "missing.mkv"), missingOutput, TimeSpan.Zero);
                throw new InvalidDataException("A missing input was accepted.");
            }
            catch (NativeEngineException)
            {
                if (File.Exists(missingOutput)) throw new InvalidDataException("Invalid input published an image.");
                cases.Add(new { name = "missing-input", status = "PASS" });
            }
            string cancelled = Path.Combine(output, "cancelled.png");
            try
            {
                await service.CreateAsync(normal, cancelled, TimeSpan.Zero, new CancellationToken(true));
                throw new InvalidDataException("A cancelled request was accepted.");
            }
            catch (OperationCanceledException)
            {
                if (File.Exists(cancelled)) throw new InvalidDataException("Cancelled request published an image.");
                cases.Add(new { name = "pre-cancel", status = "PASS" });
            }
            if (!NativeEngine.WaitForQuiescence(1000)) throw new InvalidDataException("Thumbnail jobs retained native resources.");
            Console.WriteLine(JsonSerializer.Serialize(new { status = "PASS", cases, nativeQuiescence = true }));
            return 0;
        }
        catch (Exception error)
        {
            Console.WriteLine(JsonSerializer.Serialize(new { status = "FAIL", cases, error = error.ToString() }));
            return 1;
        }
    }
}
