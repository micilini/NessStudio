using NessStudio.Recording;
using Engines = NessStudio.Recording.Engines;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers
{
    public enum RecordingExportMode
    {
        SingleFile,
        SeparateTracks
    }

    public enum RecordingExportVideoLayout
    {
        NoVideo,
        ScreenOnly,
        ScreenWebcamPictureInPicture,
        ScreenWebcamSideBySide,
        WebcamOnly,
        RecordedProgram
    }

    public enum RecordingExportAudioMode
    {
        NoAudio,
        MicrophoneOnly,
        SystemAudioOnly,
        MicrophoneAndSystemAudio
    }

    public enum RecordingExportContainer
    {
        Mp4,
        Mkv,
        Mp3,
        Wav
    }

    public enum RecordingExportQuality
    {
        Fast,
        Balanced,
        HighQuality,
        Audio128Kbps,
        Audio192Kbps,
        Audio320Kbps,
        Wav44100Hz16Bit,
        Wav48000Hz16Bit,
        Wav48000Hz24Bit
    }

    public sealed class RecordingExportRequest
    {
        public string ProjectFolder { get; set; } = string.Empty;
        public string OutputPath { get; set; } = string.Empty;
        public RecordingExportMode Mode { get; set; } = RecordingExportMode.SingleFile;
        public RecordingExportVideoLayout VideoLayout { get; set; } = RecordingExportVideoLayout.ScreenWebcamPictureInPicture;
        public RecordingExportAudioMode AudioMode { get; set; } = RecordingExportAudioMode.MicrophoneAndSystemAudio;
        public RecordingExportContainer Container { get; set; } = RecordingExportContainer.Mp4;
        public RecordingExportQuality Quality { get; set; } = RecordingExportQuality.Balanced;
        public bool ExportScreenTrack { get; set; } = true;
        public bool ExportWebcamTrack { get; set; } = true;
        public bool ExportAudioMixTrack { get; set; } = true;
        public bool ExportMicrophoneTrack { get; set; }
        public bool ExportSystemAudioTrack { get; set; }
        public bool ExportProgramTrack { get; set; }
    }

    public sealed class RecordingExportResult
    {
        public bool Success { get; set; }
        public string Message { get; set; } = string.Empty;
        public string OutputPath { get; set; } = string.Empty;
        public string Log { get; set; } = string.Empty;
        public JsonElement? NativeJob { get; set; }
        public string? ReceiptPath { get; set; }
    }

    public sealed class RecordingExportSessionInfo
    {
        public string ProjectFolder { get; set; } = string.Empty;
        public string ManifestPath { get; set; } = string.Empty;
        public SessionManifest Manifest { get; set; }
        public ProjectManifestDocument? Document { get; set; }
        public string? ScreenPath { get; set; }
        public string? WebcamPath { get; set; }
        public string? ProgramPath { get; set; }
        public string? MicPath { get; set; }
        public string? SystemPath { get; set; }

        public bool HasScreen => !string.IsNullOrWhiteSpace(ScreenPath) && File.Exists(ScreenPath);
        public bool HasWebcam => !string.IsNullOrWhiteSpace(WebcamPath) && File.Exists(WebcamPath);
        public bool HasMic => !string.IsNullOrWhiteSpace(MicPath) && File.Exists(MicPath);
        public bool HasSystemAudio => !string.IsNullOrWhiteSpace(SystemPath) && File.Exists(SystemPath);
        public bool HasProgram => !string.IsNullOrWhiteSpace(ProgramPath) && File.Exists(ProgramPath);
        public bool HasAnyVideo => HasScreen || HasWebcam || HasProgram;
        public bool HasAnyAudio => HasMic || HasSystemAudio;
    }

    /// <summary>Selects assets and persists results; all media planning/process ownership belongs to libnessstudio.</summary>
    public static class ExportService
    {
        private static readonly JsonSerializerOptions JobJson = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
        public static string FindFFmpegPath() => Path.Combine(AppContext.BaseDirectory,"Native","FFmpeg","ffmpeg.exe");
        public static RecordingExportSessionInfo LoadSessionInfo(string projectFolder)
        {
            var document=ProjectManifestStore.Read(projectFolder);var manifest=document.Manifest;
            return new() { ProjectFolder=Path.GetFullPath(projectFolder),ManifestPath=document.AuthorityPath,Manifest=manifest,Document=document,
                ScreenPath=ProjectManifestStore.ExistingPath(projectFolder,manifest.Screen?.File),WebcamPath=ProjectManifestStore.ExistingPath(projectFolder,manifest.Webcam?.File),
                ProgramPath=ProjectManifestStore.ExistingPath(projectFolder,manifest.Program?.File),MicPath=ProjectManifestStore.ExistingPath(projectFolder,manifest.Mic?.File),SystemPath=ProjectManifestStore.ExistingPath(projectFolder,manifest.System?.File) };
        }
        private static string SafeName(string value) => new(value.Select(c=>Path.GetInvalidFileNameChars().Contains(c)?'_':c).ToArray());
        public static string BuildDefaultSingleFileOutputPath(string projectFolder,RecordingExportContainer container) =>
            Path.Combine(projectFolder,"Exports",SafeName(new DirectoryInfo(projectFolder).Name)+"_export."+container.ToString().ToLowerInvariant());
        public static string BuildDefaultSeparateTracksOutputFolder(string projectFolder) => Path.Combine(projectFolder,"Exports",SafeName(new DirectoryInfo(projectFolder).Name)+"_tracks");
        /// <summary>Runs project I/O and native job preparation without blocking the calling UI thread.</summary>
        public static Task<RecordingExportResult> ExportAsync(RecordingExportRequest request,IProgress<string>? progress=null,CancellationToken cancellationToken=default)
        {
            ArgumentNullException.ThrowIfNull(request);
            // Native job creation validates inputs and hashes the distributed
            // executable synchronously. Keep that work off the Dispatcher even
            // when loading a cold package or a slow filesystem. Cancellation is
            // translated by the worker into the existing structured result.
            // Freeze the submitted options before returning to a caller that
            // may reuse its mutable request while the worker is scheduled.
            var submitted = new RecordingExportRequest
            {
                ProjectFolder=request.ProjectFolder,OutputPath=request.OutputPath,
                Mode=request.Mode,VideoLayout=request.VideoLayout,AudioMode=request.AudioMode,
                Container=request.Container,Quality=request.Quality,
                ExportScreenTrack=request.ExportScreenTrack,ExportWebcamTrack=request.ExportWebcamTrack,
                ExportAudioMixTrack=request.ExportAudioMixTrack,ExportMicrophoneTrack=request.ExportMicrophoneTrack,
                ExportSystemAudioTrack=request.ExportSystemAudioTrack,ExportProgramTrack=request.ExportProgramTrack
            };
            return Task.Run(() => ExportCoreAsync(submitted,progress,cancellationToken),CancellationToken.None);
        }
        private static async Task<RecordingExportResult> ExportCoreAsync(RecordingExportRequest request,IProgress<string>? progress,CancellationToken cancellationToken)
        {
            try
            {
                cancellationToken.ThrowIfCancellationRequested();var info=LoadSessionInfo(request.ProjectFolder);
                if(request.Mode==RecordingExportMode.SeparateTracks)return await SeparateAsync(info,request,progress,cancellationToken).ConfigureAwait(false);
                return await SingleAsync(info,request,progress,cancellationToken).ConfigureAwait(false);
            }
            catch(OperationCanceledException){return Fail("Export cancelled. Completed tracks and owned partials are preserved.",request.OutputPath);}
            catch(Exception error)when(error is IOException or InvalidDataException or InvalidOperationException or ArgumentException or JsonException or NessStudio.Interop.Native.NativeEngineException)
            {return Fail(UserError.Message(error),request.OutputPath,UserError.Details(error));}
        }
        private static long Ticks(string? duration) => TimeSpan.TryParse(duration,CultureInfo.InvariantCulture,out var time)?Math.Max(0,time.Ticks):0;
        private static object BuildProject(RecordingExportSessionInfo info,RecordingExportRequest request,bool raw)
        {
            var inputs=new List<object>();var manifest=info.Manifest;
            bool screen=request.VideoLayout is RecordingExportVideoLayout.ScreenOnly or RecordingExportVideoLayout.ScreenWebcamPictureInPicture or RecordingExportVideoLayout.ScreenWebcamSideBySide;
            bool camera=request.VideoLayout is RecordingExportVideoLayout.WebcamOnly or RecordingExportVideoLayout.ScreenWebcamPictureInPicture or RecordingExportVideoLayout.ScreenWebcamSideBySide;
            bool program=request.VideoLayout==RecordingExportVideoLayout.RecordedProgram;
            long videoDuration=0;int fps=30;
            void Video(string role,string? path,SessionManifest.VideoTrack? track)
            {
                if(path is null||track is null)throw new FileNotFoundException($"Missing {role} asset: {track?.File ?? role}. Locate the asset before exporting.");
                videoDuration=videoDuration==0?Ticks(track.Duration):videoDuration;fps=track.Fps>0?track.Fps:fps;
                inputs.Add(new { role,path,video=new { width=track.Width,height=track.Height,fps=track.Fps,crop=raw||!track.IsRawIntermediate||!track.HasDrawAreaCrop||track.CropPx is null?null:new { x=track.CropPx.X,y=track.CropPx.Y,w=track.CropPx.W,h=track.CropPx.H } },audio=(object?)null });
            }
            if(screen)Video("screen",info.ScreenPath,manifest.Screen);if(camera)Video("camera",info.WebcamPath,manifest.Webcam);if(program)Video("program",info.ProgramPath,manifest.Program);
            long audioDuration=0;
            void Audio(string role,string? path,SessionManifest.AudioTrack? track)
            {
                if(path is null||track is null)throw new FileNotFoundException($"Missing {role} asset: {track?.File ?? role}. Locate the asset before exporting.");
                long offset=Math.Max(0,track.OffsetMs??0);
                if(role=="system"&&offset==500&&manifest.Version==2&&manifest.SessionType=="nessmuxer-mkv"&&string.IsNullOrEmpty(manifest.TimingMode))offset=0;
                audioDuration=Math.Max(audioDuration,checked(Ticks(track.Duration)+offset*TimeSpan.TicksPerMillisecond));
                inputs.Add(new { role,path,video=(object?)null,audio=new { offsetMs=offset,channels=track.Channels>0?track.Channels:2 } });
            }
            if(request.AudioMode is RecordingExportAudioMode.MicrophoneOnly or RecordingExportAudioMode.MicrophoneAndSystemAudio)Audio("microphone",info.MicPath,manifest.Mic);
            if(request.AudioMode is RecordingExportAudioMode.SystemAudioOnly or RecordingExportAudioMode.MicrophoneAndSystemAudio)Audio("system",info.SystemPath,manifest.System);
            long duration=checked((manifest.DurationMs??0)*TimeSpan.TicksPerMillisecond);
            if(duration<=0)duration=videoDuration>0?videoDuration:audioDuration;
            // A single independent track can be shorter than the project. Its
            // actual timeline is authoritative; composite/audio output uses the project cut.
            if((manifest.Version==3||!string.IsNullOrEmpty(manifest.TimingMode))&&(request.VideoLayout is RecordingExportVideoLayout.ScreenOnly or RecordingExportVideoLayout.WebcamOnly or RecordingExportVideoLayout.RecordedProgram)&&videoDuration>0)duration=Math.Min(duration,videoDuration);
            Engines.RecordingScene? scene=null;
            if(!raw&&!program&&request.VideoLayout!=RecordingExportVideoLayout.NoVideo&&manifest.Scene is { } original)
            {
                var layout=request.VideoLayout switch {RecordingExportVideoLayout.ScreenOnly=>Engines.RecordingSceneLayout.Screen,RecordingExportVideoLayout.WebcamOnly=>Engines.RecordingSceneLayout.Camera,RecordingExportVideoLayout.ScreenWebcamSideBySide=>Engines.RecordingSceneLayout.SideBySide,_=>Engines.RecordingSceneLayout.PictureInPicture};
                scene=original.Layout==layout.ToString()?original:Engines.RecordingScenes.Create(layout,original.Width,original.Height,screen,camera,original.Layers.FirstOrDefault(layer=>layer.Source==2)?.Mirror??false);
            }
            return new { version=1,layout=request.VideoLayout.ToString(),container=request.Container.ToString(),quality=request.Quality.ToString(),durationHns=duration,fps=Math.Clamp(fps,1,120),inputs,scene,legacyDuration=manifest.Version==2&&string.IsNullOrEmpty(manifest.TimingMode) };
        }
        private static async Task<RecordingExportResult> SingleAsync(RecordingExportSessionInfo info,RecordingExportRequest request,IProgress<string>? progress,CancellationToken cancellationToken,bool raw=false)
        {
            if(string.IsNullOrWhiteSpace(request.OutputPath))request.OutputPath=BuildDefaultSingleFileOutputPath(info.ProjectFolder,request.Container);
            request.OutputPath=Path.GetFullPath(request.OutputPath);
            var project=BuildProject(info,request,raw); // Validate selected roles before opening any output.
            string ffmpeg=FindFFmpegPath();if(!File.Exists(ffmpeg))return Fail("The distributed FFmpeg dependency is missing: "+ffmpeg,request.OutputPath);
            Directory.CreateDirectory(Path.GetDirectoryName(request.OutputPath)!);
            if(ProjectManifestStore.Assets(info.Manifest).Any(asset=>ProjectManifestStore.ResolvePath(info.ProjectFolder,asset.File).Equals(request.OutputPath,StringComparison.OrdinalIgnoreCase)))
                return Fail("An export cannot replace a source asset. Choose another output path.",request.OutputPath);
            using var engine=NessStudio.Interop.Native.NativeEngine.Open(AppContext.BaseDirectory,"NessStudio.ProjectExport");
            if(engine.Build.AbiMinor<7)return Fail("Project exports require libnessstudio ABI 1.7 or newer.",request.OutputPath);
            await using var job=engine.CreateJob(JsonSerializer.SerializeToElement(new {version=1,type="export",profile="project-export",ffmpegPath=ffmpeg,outputPath=request.OutputPath,project},JobJson));
            Task<JsonDocument> operation=job.RunAsync(cancellationToken);long lastPercent=-1;string? lastPhase=null;
            while(!operation.IsCompleted)
            {
                using var snapshot=job.GetSnapshot();var root=snapshot.RootElement;long duration=root.GetProperty("durationHns").GetInt64();long percent=duration>0?root.GetProperty("activeTimeHns").GetInt64()*100/duration:0;
                string phase=root.GetProperty("state").GetInt32()switch{102=>"Validating",103=>"Cancelling",_=>"Exporting"};
                if(percent!=lastPercent||phase!=lastPhase){progress?.Report($"{phase} {percent}%");lastPercent=percent;lastPhase=phase;}
                await Task.WhenAny(operation,Task.Delay(100,CancellationToken.None)).ConfigureAwait(false);
            }
            using var result=await operation.ConfigureAwait(false);var value=result.RootElement;bool success=value.GetProperty("state").GetInt32()==105&&value.GetProperty("mediaComplete").GetBoolean();
            string receipt=request.OutputPath+".export.json";if(success&&(!File.Exists(request.OutputPath)||!File.Exists(receipt)))throw new InvalidDataException("Native export completed without its finalized file/receipt.");
            string message=success?"Export completed and independently decoded successfully.":value.GetProperty("state").GetInt32()==104?"Export cancelled; partial output is preserved.":"Native export failed: "+value.GetProperty("firstError").GetRawText();
            return new(){Success=success,Message=message,OutputPath=request.OutputPath,Log=value.GetRawText(),NativeJob=value.Clone(),ReceiptPath=File.Exists(receipt)?receipt:null};
        }
        private static async Task<RecordingExportResult> SeparateAsync(RecordingExportSessionInfo info,RecordingExportRequest request,IProgress<string>? progress,CancellationToken cancellationToken)
        {
            if(string.IsNullOrWhiteSpace(request.OutputPath))request.OutputPath=BuildDefaultSeparateTracksOutputFolder(info.ProjectFolder);
            Directory.CreateDirectory(request.OutputPath);var jobs=new List<(string Name,RecordingExportVideoLayout Video,RecordingExportAudioMode Audio,RecordingExportContainer Container)>();
            if(request.ExportScreenTrack&&info.HasScreen)jobs.Add(("screen.mp4",RecordingExportVideoLayout.ScreenOnly,RecordingExportAudioMode.NoAudio,RecordingExportContainer.Mp4));
            if(request.ExportWebcamTrack&&info.HasWebcam)jobs.Add(("webcam.mp4",RecordingExportVideoLayout.WebcamOnly,RecordingExportAudioMode.NoAudio,RecordingExportContainer.Mp4));
            if(request.ExportProgramTrack&&info.HasProgram)jobs.Add(("program.mp4",RecordingExportVideoLayout.RecordedProgram,RecordingExportAudioMode.NoAudio,RecordingExportContainer.Mp4));
            if(request.ExportMicrophoneTrack)jobs.Add(("microphone.wav",RecordingExportVideoLayout.NoVideo,RecordingExportAudioMode.MicrophoneOnly,RecordingExportContainer.Wav));
            if(request.ExportSystemAudioTrack)jobs.Add(("system.wav",RecordingExportVideoLayout.NoVideo,RecordingExportAudioMode.SystemAudioOnly,RecordingExportContainer.Wav));
            if(request.ExportAudioMixTrack&&request.AudioMode!=RecordingExportAudioMode.NoAudio&&info.HasAnyAudio)jobs.Add(("audio_mix.wav",RecordingExportVideoLayout.NoVideo,request.AudioMode,RecordingExportContainer.Wav));
            if(jobs.Count==0)return Fail("No available tracks were selected.",request.OutputPath);
            var receipts=new List<JsonElement>();
            foreach(var item in jobs)
            {
                cancellationToken.ThrowIfCancellationRequested();progress?.Report("Exporting "+item.Name);
                var part=new RecordingExportRequest{ProjectFolder=request.ProjectFolder,OutputPath=Path.Combine(request.OutputPath,item.Name),VideoLayout=item.Video,AudioMode=item.Audio,Container=item.Container,Quality=RecordingExportQuality.Balanced};
                var result=await SingleAsync(info,part,progress,cancellationToken,raw:true).ConfigureAwait(false);
                if(result.NativeJob is { } receipt)receipts.Add(receipt);
                if(!result.Success)return Fail("Separate export stopped: "+result.Message,request.OutputPath,JsonSerializer.Serialize(receipts));
            }
            return new(){Success=true,Message="All selected tracks were exported and decoded successfully.",OutputPath=request.OutputPath,Log=JsonSerializer.Serialize(receipts)};
        }
        private static RecordingExportResult Fail(string message,string path,string log="")=>new(){Success=false,Message=message,OutputPath=path,Log=log};
    }
}
