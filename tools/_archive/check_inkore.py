import clr
import System.Reflection

path = r'C:/Users/Creatinx/.nuget/packages/inkore.ui.wpf.modern/0.10.2.1/lib/net6.0-windows7.0/iNKORE.UI.WPF.Modern.dll'
asm = System.Reflection.Assembly.LoadFrom(path)

types = [t.FullName for t in asm.GetTypes() if 'Theme' in t.FullName or 'WindowEx' in t.FullName or 'ThemeKeys' in t.FullName]
for t in types[:30]:
    print(t)
