using System; using System.Threading; using System.Text;
public class Fixture { public static void Main(string[] a) { var marker=Encoding.ASCII.GetBytes("RYGT_FATAL_WATCHER_PRIVATE_HEAP_MARKER_8A3E06D51A6A7B2F"); Console.WriteLine("ready " + System.Diagnostics.Process.GetCurrentProcess().Id); Console.Out.Flush(); Thread.Sleep(60000); GC.KeepAlive(marker); } }
