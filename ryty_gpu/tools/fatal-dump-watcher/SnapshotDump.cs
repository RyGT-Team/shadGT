using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Diagnostics;

public static class SnapshotDump {
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint a,bool i,int pid);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetProcessTimes(IntPtr p,out long creation,out long exit,out long kernel,out long user);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr p,int flags,System.Text.StringBuilder path,ref int size);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 [DllImport("kernel32.dll")] static extern uint PssCaptureSnapshot(IntPtr p,uint flags,uint context,out IntPtr snap);
 [DllImport("kernel32.dll")] static extern uint PssFreeSnapshot(IntPtr p,IntPtr snap);
 [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandle(string name);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern int GetModuleFileName(IntPtr module,System.Text.StringBuilder path,int size);
 [UnmanagedFunctionPointer(CallingConvention.Winapi)] delegate bool Callback(IntPtr param,IntPtr input,IntPtr output);
 [StructLayout(LayoutKind.Sequential)] struct Callbacks { public Callback cb; public IntPtr param; }
 [DllImport("dbghelp.dll",SetLastError=true)] static extern bool MiniDumpWriteDump(IntPtr snapshot,int pid,IntPtr file,uint type,IntPtr exception,IntPtr user,ref Callbacks cb);
 const uint ContextAllAmd64=0x0010001f;
 static DateTime deadline; static FileStream dumpFile; static long maxBytes,previousBytes; static bool cancelled;
 static bool OnCallback(IntPtr p,IntPtr input,IntPtr output) { int type=Marshal.ReadInt32(input, IntPtr.Size==8 ? 12 : 8); if(type==16) Marshal.WriteInt32(output,1); if(type==6) { Marshal.WriteInt32(output,0,1); bool cancel=DateTime.UtcNow >= deadline; try { cancel |= dumpFile.Length > maxBytes-previousBytes; } catch { cancel=true; } cancelled |= cancel; Marshal.WriteInt32(output,4,cancel ? 1 : 0); } return true; }
 public static string Capture(int pid,string expectedExe,long expectedStart,string output,int seconds,long limitBytes) {
   if(IntPtr.Size!=8) throw new PlatformNotSupportedException("AMD64 capture requires a 64-bit worker");
   deadline=DateTime.UtcNow.AddSeconds(seconds); maxBytes=limitBytes; previousBytes=0; cancelled=false;
   using(var process=Process.GetProcessById(pid)) {
    if (!String.Equals(Path.GetFullPath(process.MainModule.FileName),Path.GetFullPath(expectedExe),StringComparison.OrdinalIgnoreCase) || process.StartTime.ToUniversalTime().ToFileTimeUtc()!=expectedStart) throw new Exception("Process identity mismatch");
    IntPtr h=OpenProcess(0x0400|0x0010|0x0040|0x0080,false,pid); if(h==IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
    long creation, exit, kernel, user; var path=new System.Text.StringBuilder(32768); int size=path.Capacity; if(!GetProcessTimes(h,out creation,out exit,out kernel,out user) || !QueryFullProcessImageName(h,0,path,ref size) || creation!=expectedStart || !String.Equals(Path.GetFullPath(path.ToString()),Path.GetFullPath(expectedExe),StringComparison.OrdinalIgnoreCase)) { CloseHandle(h); throw new Exception("Opened process handle identity mismatch"); }
    IntPtr snap=IntPtr.Zero;
    try {
      // VA clone, VA metadata, threads and contexts. Snapshot first, dump clone.
      uint e=PssCaptureSnapshot(h,0x00000001|0x00000800|0x00000080|0x00000100,ContextAllAmd64,out snap); if(e!=0) throw new System.ComponentModel.Win32Exception((int)e,"PssCaptureSnapshot error "+e);
      File.WriteAllText(output+".arguments.txt","PssCaptureFlags=0x981\nThreadContextFlags=0x0010001f\nPrimaryMiniDumpType=0x1a00\nFallbackMiniDumpType=0x0\n");
      int error=0; string selected=output;
      for(int attempt=0;attempt<2;attempt++) {
       if(DateTime.UtcNow>=deadline || previousBytes>=maxBytes) throw new Exception("Dump budget exhausted");
       File.AppendAllText(output+".attempts.txt",attempt==0 ? "primary=0x1a00\n" : "fallback=0x0\n");
       bool ok;
       using(var file=new FileStream(selected,FileMode.CreateNew,FileAccess.Write,FileShare.Read)) {
        dumpFile=file; Callback callback=OnCallback; var cb=new Callbacks{cb=callback,param=IntPtr.Zero};
        ok=MiniDumpWriteDump(snap,pid,file.SafeFileHandle.DangerousGetHandle(),attempt==0 ? 0x200u|0x800u|0x1000u : 0u,IntPtr.Zero,IntPtr.Zero,ref cb);
        error=ok ? 0 : Marshal.GetLastWin32Error(); file.Flush(true); previousBytes+=file.Length; GC.KeepAlive(callback);
       }
       {
        var modulePath=new System.Text.StringBuilder(32768); var module=GetModuleHandle("dbghelp.dll");
        if(module!=IntPtr.Zero && GetModuleFileName(module,modulePath,modulePath.Capacity)>0) {
         string loaded=modulePath.ToString(); string version; try { version=FileVersionInfo.GetVersionInfo(loaded).FileVersion; } catch { version="unavailable"; }
         File.WriteAllText(output+".dbghelp.txt","path="+loaded+Environment.NewLine+"version="+version);
        } else File.WriteAllText(output+".dbghelp.txt","Loaded dbghelp path unavailable");
       }
       if(ok) { if(cancelled || previousBytes>maxBytes || DateTime.UtcNow>=deadline) throw new Exception("Dump budget exhausted"); return selected; }
       if(attempt!=0 || cancelled || error!=unchecked((int)0x80070057)) throw new Exception("MiniDumpWriteDump error 0x"+error.ToString("x"));
       File.WriteAllText(output+".primary-error.txt","MiniDumpWriteDump error 0x"+error.ToString("x"));
       selected=Path.Combine(Path.GetDirectoryName(output),Path.GetFileNameWithoutExtension(output)+"-fallback.dmp");
      }
      throw new Exception("Dump failed");
    } finally { uint freeResult=0; if(snap!=IntPtr.Zero) freeResult=PssFreeSnapshot(GetCurrentProcess(),snap); CloseHandle(h); if(freeResult!=0) throw new Exception("PssFreeSnapshot error "+freeResult); }
   }
 }
}







