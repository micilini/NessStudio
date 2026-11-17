using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using SQLite;
namespace NessStudio.ViewModel.Helpers
{
    public class DatabaseHelper
    {
        private static Func<SQLiteConnection>? _connectionProvider;
        public static void Configure(Func<SQLiteConnection> connectionProvider)
            => _connectionProvider = connectionProvider ?? throw new ArgumentNullException(nameof(connectionProvider));
        private static SQLiteConnection Connection => _connectionProvider?.Invoke()
            ?? throw new InvalidOperationException("Database access must be configured by the application composition root.");
        public static bool Insert<T>(T item)
        {
            bool result = false;
            var conn = Connection;
            conn.RunInTransaction(() =>
            {
                conn.CreateTable<T>();
                int rows = conn.Insert(item);
                if (rows > 0)
                    result = true;
            });
            return result;
        }
        /// <summary>Registers media idempotently in one transaction without overwriting an existing project's title or preferences.</summary>
        public static NessStudio.Models.ProjectsModel RegisterProject(NessStudio.Models.ProjectsModel project)
        {
            var connection=Connection;var result=project;
            connection.RunInTransaction(()=>
            {
                connection.CreateTable<NessStudio.Models.ProjectsModel>();
                var existing=connection.Table<NessStudio.Models.ProjectsModel>().Where(item=>item.ProjectId==project.ProjectId||item.ProjectFolderPath==project.ProjectFolderPath).FirstOrDefault();
                if(existing is not null)
                {
                    if(!System.IO.Directory.Exists(existing.ProjectFolderPath)&&System.IO.Directory.Exists(project.ProjectFolderPath))
                    {existing.ProjectFolderPath=project.ProjectFolderPath;existing.ProjectFilePath=project.ProjectFilePath;existing.ThumbnailPath=project.ThumbnailPath;connection.Update(existing);}
                    result=existing;
                }
                else connection.Insert(project);
            });
            return result;
        }
        public static bool Update<T>(T item)
        {
            bool result = false;
            var conn = Connection;
            conn.RunInTransaction(() =>
            {
                conn.CreateTable<T>();
                int rows = conn.Update(item);
                if (rows > 0)
                    result = true;
            });
            return result;
        }
        public static bool Delete<T>(T item)
        {
            bool result = false;
            var conn = Connection;
            conn.RunInTransaction(() =>
            {
                conn.CreateTable<T>();
                int rows = conn.Delete(item);
                if (rows > 0)
                    result = true;
            });
            return result;
        }
        public static List<T> Read<T>() where T : new()
        {
            List<T> items = new List<T>();
            var conn = Connection;
            conn.RunInTransaction(() =>
            {
                conn.CreateTable<T>();
                items = conn.Table<T>().ToList();
            });
            return items;
        }
        public static int Execute(string query, params object[] args)
        {
            int result = 0;
            var conn = Connection;
            conn.RunInTransaction(() =>
            {
                result = conn.Execute(query, args);
            });
            return result;
        }
        public static T QuerySingle<T>(string query, params object[] args) where T : new()
        {
            T result = default;
            var conn = Connection;
            conn.RunInTransaction(() =>
            {
                var queryResult = conn.Query<T>(query, args);
                result = queryResult.FirstOrDefault();
            });
            return result;
        }
    }
}