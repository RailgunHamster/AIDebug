using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
namespace SboxDebugFixture;
public static class Probe {
    [MethodImpl(MethodImplOptions.NoInlining)] public static int Add(int value)=>value+7;
    [MethodImpl(MethodImplOptions.NoInlining)] public static int Throws()=>throw new InvalidOperationException("fixture");
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate long Integer(long value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate double Floating(double a,double b,double c,double d);
    public static Integer? NativeInteger;
    public static Floating? NativeFloating;
    public static void LoadNative(string path){var module=NativeLibrary.Load(path);NativeInteger=Marshal.GetDelegateForFunctionPointer<Integer>(NativeLibrary.GetExport(module,"probe_integer"));NativeFloating=Marshal.GetDelegateForFunctionPointer<Floating>(NativeLibrary.GetExport(module,"probe_four_doubles"));}
}