using NessStudio.Models;
using NessStudio.Recording;
using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;

namespace NessStudio.ViewModel.Helpers;

/// <summary>Imports project metadata into the existing database without mutating old projects on open.</summary>
public static class ProjectIngestService
{
    public static async Task<ProjectsModel> ProcessAsync(string projectFolder,IProgress<RecordingSaveProgress>? progress=null)
    {
        if(!Directory.Exists(projectFolder))throw new DirectoryNotFoundException("Project folder not found: "+projectFolder);
        var document=ProjectManifestStore.Read(projectFolder);var manifest=document.Manifest;
        string projectId=manifest.ProjectId??(string.IsNullOrWhiteSpace(manifest.SessionId)?Guid.NewGuid().ToString("N"):manifest.SessionId);
        try
        {
            void Report(string message,int percent)=>progress?.Report(new(){Title="Saving Recording...",Message=message,Percent=percent,CurrentStep=percent<98?7:8,TotalSteps=8});
            string? preview=ProjectManifestStore.ExistingPath(projectFolder,manifest.Thumbnail)??ProjectManifestStore.ExistingPath(projectFolder,"preview.png");
            string? video=ResolvePreviewVideoFile(projectFolder,manifest);
            Report("Generating preview...",95);
            if(preview is null&&video is not null)
            {
                string output=Path.Combine(projectFolder,"preview.png");
                try
                {
                    using var deadline=new System.Threading.CancellationTokenSource(TimeSpan.FromSeconds(5));
                    await new NativeThumbnailService(AppContext.BaseDirectory).CreateAsync(video,output,TimeSpan.FromSeconds(2),deadline.Token);
                    if(File.Exists(output))preview=output;
                }
                catch(Exception error){DebugLog.Write("[ProjectIngest] Thumbnail: "+error);}
            }
            // This is the explicit save/import path. Opening or listing projects does not write aliases.
            if(manifest.Version==3&&preview is not null&&manifest.Thumbnail is null)
            {manifest.Thumbnail=Path.GetRelativePath(projectFolder,preview);ProjectManifestStore.WriteAliases(projectFolder,manifest,overwrite:true);}
            var track=manifest.HasProgram?manifest.Program:manifest.HasScreen?manifest.Screen:manifest.HasWebcam?manifest.Webcam:null;
            var model=new ProjectsModel{ProjectId=projectId,Title=new DirectoryInfo(projectFolder).Name,ProjectFolderPath=projectFolder,
                ProjectFilePath=document.AuthorityPath,ThumbnailPath=preview??string.Empty,FileSizeBytes=FolderSize(projectFolder),DurationMs=manifest.DurationMs??0,
                VideoWidth=track?.Width??0,VideoHeight=track?.Height??0,FrameRate=track?.Fps??0,HasMicrophoneAudio=manifest.HasMic,HasSystemAudio=manifest.HasSystemAudio,HasWebcam=manifest.HasWebcam,
                CreatedAt=manifest.CreatedAt==default?DateTime.Now:manifest.CreatedAt.LocalDateTime,UpdatedAt=DateTime.Now,LastOpenedAt=DateTime.Now};
            model.Title = model.DisplayTitle;
            Report("Registering project...",98);var registered=DatabaseHelper.RegisterProject(model);
            if(manifest.Version==3)ProjectManifestStore.RecordRegistration(projectFolder,projectId,true);
            Report("Finishing...",100);return registered;
        }
        catch(Exception error)
        {
            if(manifest.Version==3)
            {try{ProjectManifestStore.RecordRegistration(projectFolder,projectId,false,error.Message);}catch(Exception metadataError){DebugLog.Write("[ProjectIngest] Registration recovery: "+metadataError);}}
            throw;
        }
    }
    private static string? ResolvePreviewVideoFile(string directory,SessionManifest manifest)
    {
        if (manifest.Version == 3 && !manifest.HasProgram && !manifest.HasScreen && !manifest.HasWebcam)
            return null;
        foreach(string? file in new[]{manifest.PrimaryAsset,manifest.Program?.File,manifest.Screen?.File,manifest.Webcam?.File})
        {string? path=ProjectManifestStore.ExistingPath(directory,file);if(path is not null&&new[]{".mkv",".mp4",".mov",".avi"}.Contains(Path.GetExtension(path).ToLowerInvariant()))return path;}
        if(manifest.Version==2)
        {
            foreach(string file in new[]{"screen.mkv","webcam.mp4","screen_01.mp4","webcam_01.mp4"})
            {string? path=ProjectManifestStore.ExistingPath(directory,file);if(path is not null)return path;}
            return Directory.EnumerateFiles(directory,"*.mp4",SearchOption.TopDirectoryOnly).OrderBy(path=>path,StringComparer.OrdinalIgnoreCase).FirstOrDefault();
        }
        return null;
    }
    private static long FolderSize(string directory)
    {
        long size=0;foreach(string file in Directory.EnumerateFiles(directory,"*",SearchOption.AllDirectories))
        {try{size=checked(size+new FileInfo(file).Length);}catch(IOException){}}
        return size;
    }
}
