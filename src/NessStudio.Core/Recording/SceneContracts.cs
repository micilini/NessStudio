namespace NessStudio.Recording.Engines;

/// <summary>Chooses the assets retained by a native session.</summary>
public enum RecordingOutputMode
{
    /// <summary>Keeps every selected source for subsequent editing.</summary>
    SeparateSources,
    /// <summary>Keeps only the explicitly composed program video and selected audio.</summary>
    Program,
    /// <summary>Keeps program and independent sources after all encoders prepare.</summary>
    ProgramAndSources
}

/// <summary>Defines the supported scene arrangements.</summary>
public enum RecordingSceneLayout
{
    /// <summary>Displays the screen.</summary>
    Screen,
    /// <summary>Displays the camera.</summary>
    Camera,
    /// <summary>Places the camera over the lower right of the screen.</summary>
    PictureInPicture,
    /// <summary>Places screen and camera in equal adjacent regions.</summary>
    SideBySide
}

/// <summary>Defines a normalized rectangle inside a source or canvas.</summary>
public sealed record SceneRectangle(double X = 0, double Y = 0, double Width = 1, double Height = 1);

/// <summary>Describes one source layer; no frame, resource or native handle crosses this contract.</summary>
public sealed record SceneLayer([property:System.Text.Json.Serialization.JsonPropertyName("sourceId")] ulong Source, SceneRectangle Crop, SceneRectangle Destination,
    bool Visible = true, double Opacity = 1, int Rotation = 0, bool Mirror = false,
    string Fit = "Contain", string Missing = "HoldLast", int MaxAgeMs = 500, bool Required = true);

/// <summary>Describes an immutable SDR program canvas. Native updates keep dimensions and cadence fixed.</summary>
public sealed record RecordingScene(int Width, int Height, string Layout, IReadOnlyList<SceneLayer> Layers,
    int FpsNum = 30, int FpsDen = 1)
{
    /// <summary>Gets the native scene contract version.</summary>
    public int Version => 1;
    /// <summary>Gets the explicit output color contract.</summary>
    public string Color => "BT709Limited";
    /// <summary>Gets the opaque black RGB background.</summary>
    public IReadOnlyList<int> Background => [0, 0, 0];
}

/// <summary>Defines the shared geometry used by capture, preview and export.</summary>
public static class RecordingScenes
{
    /// <summary>Creates one of the canonical layouts while retaining hidden source identities for live changes.</summary>
    public static RecordingScene Create(RecordingSceneLayout layout, int width, int height, bool screen, bool camera, bool mirrorCamera = false)
    {
        if (width is <= 0 or > 1920 || height is <= 0 or > 1080 || (width & 1) != 0 || (height & 1) != 0)
            throw new ArgumentOutOfRangeException(nameof(width), "Scene dimensions must be positive, even and within 1920x1080.");
        if (!screen && !camera || layout == RecordingSceneLayout.Screen && !screen || layout == RecordingSceneLayout.Camera && !camera ||
            layout is RecordingSceneLayout.PictureInPicture or RecordingSceneLayout.SideBySide && (!screen || !camera))
            throw new ArgumentException("The selected scene layout requires the corresponding sources.");
        List<SceneLayer> layers = [];
        if (screen) layers.Add(new(1, new(), layout == RecordingSceneLayout.SideBySide ? new(0, 0, .5, 1) : new(), layout != RecordingSceneLayout.Camera));
        if (camera) layers.Add(new(2, new(), layout switch
        {
            RecordingSceneLayout.PictureInPicture => new(.70, .68, .28, .30),
            RecordingSceneLayout.SideBySide => new(.5, 0, .5, 1),
            _ => new()
        }, layout != RecordingSceneLayout.Screen, Mirror: mirrorCamera));
        return new(width, height, layout.ToString(), layers.AsReadOnly());
    }

    /// <summary>Resolves even destination pixels and an explicit source crop using the native scene rules.</summary>
    public static (int X, int Y, int Width, int Height, SceneRectangle Crop) Place(RecordingScene scene, SceneLayer layer, int sourceWidth, int sourceHeight)
    {
        static int Even(double value) => checked((int)Math.Round(value / 2, MidpointRounding.AwayFromZero) * 2);
        int x = Math.Clamp(Even(layer.Destination.X * scene.Width), 0, scene.Width - 2);
        int y = Math.Clamp(Even(layer.Destination.Y * scene.Height), 0, scene.Height - 2);
        int width = Math.Clamp(Even(layer.Destination.Width * scene.Width), 2, scene.Width - x);
        int height = Math.Clamp(Even(layer.Destination.Height * scene.Height), 2, scene.Height - y);
        double sw = sourceWidth * layer.Crop.Width, sh = sourceHeight * layer.Crop.Height;
        if (layer.Rotation is 90 or 270) (sw, sh) = (sh, sw);
        var crop = layer.Crop;
        if (layer.Fit == "Cover")
        {
            double amount = (double)width / height / (sw / sh);
            if (layer.Rotation is 90 or 270) amount = 1 / amount;
            if (amount < 1) crop = crop with { X = crop.X + crop.Width * (1 - amount) / 2, Width = crop.Width * amount };
            else crop = crop with { Y = crop.Y + crop.Height * (1 - 1 / amount) / 2, Height = crop.Height / amount };
        }
        else
        {
            double scale = Math.Min(width / sw, height / sh);
            int fitWidth = Math.Clamp(Even(sw * scale), 2, width), fitHeight = Math.Clamp(Even(sh * scale), 2, height);
            x += Even((width - fitWidth) / 2.0); y += Even((height - fitHeight) / 2.0); width = fitWidth; height = fitHeight;
        }
        return (x, y, width, height, crop);
    }
}
