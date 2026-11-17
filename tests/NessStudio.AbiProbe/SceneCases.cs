using System.Text.Json;
using NessStudio.Recording.Engines;

internal static class SceneCases
{
    internal static int Run()
    {
        try
        {
            int cases=0;
            void Check(bool condition,string message) { if(!condition)throw new InvalidDataException(message);cases++; }
            foreach(var layout in Enum.GetValues<RecordingSceneLayout>())
            {
                var scene=RecordingScenes.Create(layout,1280,720,true,true,true);
                using var json=JsonDocument.Parse(JsonSerializer.Serialize(scene,new JsonSerializerOptions { PropertyNamingPolicy=JsonNamingPolicy.CamelCase }));
                Check(json.RootElement.GetProperty("layers")[0].GetProperty("sourceId").GetUInt64()==1,"Scene source ABI identity was lost.");
                Check(scene.Layers.Single(layer=>layer.Source==2).Mirror,"The explicit saved-program mirror was lost.");
                Check(scene.Layers.Count==2,"Hidden source identities were removed, preventing live changes.");
                foreach(var layer in scene.Layers)
                {
                    var place=RecordingScenes.Place(scene,layer,1920,1200);
                    Check(place.X>=0&&place.Y>=0&&place.Width>0&&place.Height>0&&place.X+place.Width<=scene.Width&&place.Y+place.Height<=scene.Height&&
                        (place.X|place.Y|place.Width|place.Height)%2==0,"Shared scene geometry escaped its even canvas.");
                }
            }
            var full=RecordingScenes.Create(RecordingSceneLayout.Screen,320,180,true,false);
            var geometry=RecordingScenes.Place(full,full.Layers[0],1920,1200);
            Check(geometry.X==16&&geometry.Y==0&&geometry.Width==288&&geometry.Height==180,"Native/export contain geometry differs.");
            geometry=RecordingScenes.Place(full,full.Layers[0] with {Rotation=90},320,180);
            Check(geometry.Width==102&&geometry.Height==180,"Native/export rotation geometry differs.");
            geometry=RecordingScenes.Place(full,full.Layers[0] with {Fit="Cover"},640,180);
            Check(Math.Abs(geometry.Crop.X-.25)<.000001&&Math.Abs(geometry.Crop.Width-.5)<.000001,"Native/export cover geometry differs.");
            Console.WriteLine(JsonSerializer.Serialize(new {status="PASS",cases,scope="UI-free scene contracts and canonical export geometry"}));return 0;
        }
        catch(Exception error){Console.Error.WriteLine(error);return 1;}
    }
}
