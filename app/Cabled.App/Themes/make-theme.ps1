<#
  Writes Mangrove.xaml from the Mangrove Glass tokens (https://github.com/doff256/mangrove-glass).
  Theme-dependent things (colours, brushes, WinUI overrides) are repeated inside each theme
  dictionary, which is what makes them follow light/dark switches. Run after changing a value.
#>
$ErrorActionPreference = 'Stop'
$themes = [ordered]@{
  Default = [ordered]@{ Surface000='#FF0B0C0D'; Surface100='#FF131415'; Surface200='#FF1A1B1D'; Surface300='#FF232527'; Glass='#9E202225'; GlassEdge='#1FFFFFFF'; Line='#FF2A2C2F'
    Ink='#FFECE8E4'; InkMuted='#FFA39D97'; InkFaint='#FF6B6661'; Mangrove='#FF8E3B33'; MangroveDeep='#FF4B1F1B'; MangroveGlow='#FFD9735F'; OnMangrove='#FFF7EBE8'
    Bark='#FF3B2A24'; Leaf='#FF6E8F4E'; Tide='#FF5FA8B8'; Clay='#FFCF8452'; Rose='#FFE0697C'; WindowTint='#E6131415'; WellEdge='#FF050606'; GlassTint='#FF202225'; GlassOpacity='0.62'; RaiseEdge='#12FFFFFF'; RaiseShadow='#73000000'; WellShadow='#A6000000'; WellLip='#0DFFFFFF'; Clear='#00000000' }
  Light = [ordered]@{ Surface000='#FFE7E3DE'; Surface100='#FFF3F1EE'; Surface200='#FFFBFAF8'; Surface300='#FFE6E2DD'; Glass='#A3FFFFFF'; GlassEdge='#F2FFFFFF'; Line='#FFD9D4CE'
    Ink='#FF1B1716'; InkMuted='#FF5C5550'; InkFaint='#FF8F8780'; Mangrove='#FF8E3B33'; MangroveDeep='#FF6E2A24'; MangroveGlow='#FF9A3F35'; OnMangrove='#FFF7EBE8'
    Bark='#FFC9B3A8'; Leaf='#FF4E6E33'; Tide='#FF2F6F7D'; Clay='#FF9C5226'; Rose='#FFA8324A'; WindowTint='#E6F3F1EE'; WellEdge='#FFD3CEC8'; GlassTint='#FFFFFFFF'; GlassOpacity='0.64'; RaiseEdge='#E6FFFFFF'; RaiseShadow='#2E3C2823'; WellShadow='#333C2823'; WellLip='#CCFFFFFF'; Clear='#00FFFFFF' }
  HighContrast = [ordered]@{ Surface000='#FF000000'; Surface100='#FF000000'; Surface200='#FF000000'; Surface300='#FF000000'; Glass='#FF000000'; GlassEdge='#FFFFFFFF'; Line='#FFFFFFFF'
    Ink='#FFFFFFFF'; InkMuted='#FFFFFFFF'; InkFaint='#FFFFFFFF'; Mangrove='#FF8E3B33'; MangroveDeep='#FF4B1F1B'; MangroveGlow='#FFFF9A86'; OnMangrove='#FFFFFFFF'
    Bark='#FFFFFFFF'; Leaf='#FFFFFFFF'; Tide='#FF7FD3E6'; Clay='#FFFFB27A'; Rose='#FFFF8FA0'; WindowTint='#FF000000'; WellEdge='#FFFFFFFF'; GlassTint='#FF000000'; GlassOpacity='1'; RaiseEdge='#00000000'; RaiseShadow='#00000000'; WellShadow='#00000000'; WellLip='#FFFFFFFF'; Clear='#00000000' }
}
# WinUI keys -> token brushes (stock controls take the system's look).
$aliases = [ordered]@{
  ButtonBackground='Surface300'; ButtonBackgroundPointerOver='Surface300'; ButtonBackgroundPressed='Surface000'; ButtonBorderBrush='Line'; ButtonBorderBrushPointerOver='Line'; ButtonForeground='Ink'
  AccentButtonBackground='MangroveLacquer'; AccentButtonBackgroundPointerOver='Mangrove'; AccentButtonBackgroundPressed='MangroveDeep'
  AccentButtonForeground='OnMangrove'; AccentButtonForegroundPointerOver='OnMangrove'; AccentButtonForegroundPressed='OnMangrove'
  AccentButtonBorderBrush='MangroveDeep'; AccentButtonBorderBrushPointerOver='MangroveDeep'; AccentButtonBorderBrushPressed='MangroveDeep'
  AccentFillColorDefaultBrush='Mangrove'; AccentFillColorSecondaryBrush='Mangrove'; AccentFillColorTertiaryBrush='MangroveDeep'
  AccentTextFillColorPrimaryBrush='MangroveGlow'; AccentTextFillColorSecondaryBrush='MangroveGlow'; AccentTextFillColorTertiaryBrush='MangroveGlow'
  TextOnAccentFillColorPrimaryBrush='OnMangrove'; TextFillColorPrimaryBrush='Ink'; TextFillColorSecondaryBrush='InkMuted'; TextFillColorTertiaryBrush='InkFaint'
  SystemFillColorSuccessBrush='Tide'; SystemFillColorCautionBrush='Clay'; SystemFillColorCriticalBrush='Rose'
  ToggleSwitchFillOn='Mangrove'; ToggleSwitchFillOnPointerOver='Mangrove'; ToggleSwitchFillOnPressed='MangroveDeep'
  ToggleSwitchKnobFillOn='OnMangrove'; ToggleSwitchKnobFillOnPointerOver='OnMangrove'; ToggleSwitchKnobFillOnPressed='OnMangrove'
  ToggleSwitchFillOff='Surface000'; ToggleSwitchFillOffPointerOver='Surface000'; ToggleSwitchStrokeOff='Line'; ToggleSwitchStrokeOffPointerOver='InkFaint'
  ToggleSwitchKnobFillOff='InkMuted'; ToggleSwitchKnobFillOffPointerOver='Ink'
  SliderTrackFill='Surface000'; SliderTrackFillPointerOver='Surface000'; SliderTrackFillPressed='Surface000'
  SliderTrackValueFill='MangroveFill'; SliderTrackValueFillPointerOver='MangroveFill'; SliderTrackValueFillPressed='MangroveGlow'
  SliderThumbBackground='Ink'; SliderThumbBackgroundPointerOver='Ink'; SliderThumbBackgroundPressed='OnMangrove'; SliderOuterThumbBackground='Surface300'
  CheckBoxCheckBackgroundFillChecked='Mangrove'; CheckBoxCheckBackgroundStrokeChecked='Mangrove'; CheckBoxCheckGlyphForegroundChecked='OnMangrove'
  ComboBoxBackground='Surface300'; ComboBoxBorderBrush='Line'; TextControlBackground='Surface300'; TextControlBorderBrush='Line'
  ListViewItemBackgroundSelected='Surface300'; ListViewItemBackgroundSelectedPointerOver='Surface300'
  ToggleButtonBackground='Surface300'; ToggleButtonBackgroundPointerOver='Surface300'; ToggleButtonBackgroundPressed='Surface000'; ToggleButtonForeground='Ink'
  ToggleButtonBorderBrush='Line'; ToggleButtonBorderBrushPointerOver='Line'
  ToggleButtonBackgroundChecked='MangroveLacquer'; ToggleButtonBackgroundCheckedPointerOver='Mangrove'; ToggleButtonBackgroundCheckedPressed='MangroveDeep'
  ToggleButtonForegroundChecked='OnMangrove'; ToggleButtonForegroundCheckedPointerOver='OnMangrove'; ToggleButtonForegroundCheckedPressed='OnMangrove'
  ToggleButtonBorderBrushChecked='MangroveDeep'; ToggleButtonBorderBrushCheckedPointerOver='MangroveDeep'
  TextControlBackgroundPointerOver='Surface300'; TextControlBackgroundFocused='Surface000'; TextControlBorderBrushFocused='MangroveGlow'
  TextControlForeground='Ink'; TextControlForegroundFocused='Ink'; TextControlPlaceholderForeground='InkMuted'
  FocusStrokeColorOuterBrush='MangroveGlow'; FocusStrokeColorInnerBrush='Surface100'
  SystemControlFocusVisualPrimaryBrush='MangroveGlow'; SystemControlFocusVisualSecondaryBrush='Surface100'
  SolidBackgroundFillColorBaseBrush='Surface100'; CardBackgroundFillColorDefaultBrush='Surface200'; CardStrokeColorDefaultBrush='Line'
  ControlStrokeColorDefaultBrush='Line'; DividerStrokeColorDefaultBrush='Line'
}
# WinUI colour keys -> token colours (the system accent is a Color, not a brush).
$colorAliases = [ordered]@{
  SystemAccentColor='Mangrove'; SystemAccentColorLight1='MangroveGlow'; SystemAccentColorLight2='MangroveGlow'; SystemAccentColorLight3='MangroveGlow'
  SystemAccentColorDark1='Mangrove'; SystemAccentColorDark2='MangroveDeep'; SystemAccentColorDark3='MangroveDeep'
  FocusStrokeColorOuter='MangroveGlow'; FocusStrokeColorInner='Surface100'
}
$notColors = 'GlassTint', 'GlassOpacity', 'Clear'
$sb = New-Object System.Text.StringBuilder
foreach ($name in $themes.Keys) {
  $c = $themes[$name]
  [void]$sb.AppendLine("        <ResourceDictionary x:Key=`"$name`">")
  $keys = @($c.Keys | Where-Object { $_ -notin $notColors })
  foreach ($k in $keys) { [void]$sb.AppendLine("            <Color x:Key=`"$($k)Color`">$($c[$k])</Color>") }
  foreach ($k in $keys) { [void]$sb.AppendLine("            <SolidColorBrush x:Key=`"$($k)Brush`" Color=`"$($c[$k])`" />") }
  foreach ($k in $colorAliases.Keys) { [void]$sb.AppendLine("            <Color x:Key=`"$k`">$($c[$colorAliases[$k]])</Color>") }
  # Glass: in-app acrylic, so it blurs what's behind it in the window (the camera preview).
  [void]$sb.AppendLine("            <AcrylicBrush x:Key=`"GlassAcrylicBrush`" TintColor=`"$($c.GlassTint)`" TintOpacity=`"$($c.GlassOpacity)`" FallbackColor=`"$($c.Glass)`" />")
  [void]$sb.AppendLine("            <LinearGradientBrush x:Key=`"MangroveLacquerBrush`" StartPoint=`"0,0`" EndPoint=`"0,1`"><GradientStop Offset=`"0`" Color=`"$($c.Mangrove)`" /><GradientStop Offset=`"1.4`" Color=`"$($c.MangroveDeep)`" /></LinearGradientBrush>")
  [void]$sb.AppendLine("            <LinearGradientBrush x:Key=`"MangroveFillBrush`" StartPoint=`"0,0`" EndPoint=`"1,0`"><GradientStop Offset=`"0`" Color=`"$($c.MangroveDeep)`" /><GradientStop Offset=`"1`" Color=`"$($c.Mangrove)`" /></LinearGradientBrush>")
  # bundle.css materials that need gradients: toggle track (on), knobs and thumbs (raised), lacquer
  # highlight, and the inner shadow of a carved well (shadow-well has no XAML equivalent, so a top shade).
  [void]$sb.AppendLine("            <LinearGradientBrush x:Key=`"MangroveTrackBrush`" StartPoint=`"0,0`" EndPoint=`"0,1`"><GradientStop Offset=`"0`" Color=`"$($c.MangroveDeep)`" /><GradientStop Offset=`"1`" Color=`"$($c.Mangrove)`" /></LinearGradientBrush>")
  [void]$sb.AppendLine("            <LinearGradientBrush x:Key=`"KnobBrush`" StartPoint=`"0,0`" EndPoint=`"0,1`"><GradientStop Offset=`"0`" Color=`"$($c.Ink)`" /><GradientStop Offset=`"1.6`" Color=`"$($c.InkMuted)`" /></LinearGradientBrush>")
  [void]$sb.AppendLine("            <LinearGradientBrush x:Key=`"KnobOnBrush`" StartPoint=`"0,0`" EndPoint=`"0,1`"><GradientStop Offset=`"0`" Color=`"$($c.OnMangrove)`" /><GradientStop Offset=`"2.2`" Color=`"$($c.InkMuted)`" /></LinearGradientBrush>")
  [void]$sb.AppendLine("            <LinearGradientBrush x:Key=`"ThumbBrush`" StartPoint=`"0,0`" EndPoint=`"0,1`"><GradientStop Offset=`"0`" Color=`"$($c.Ink)`" /><GradientStop Offset=`"1.7`" Color=`"$($c.InkMuted)`" /></LinearGradientBrush>")
  [void]$sb.AppendLine("            <LinearGradientBrush x:Key=`"LacquerShineBrush`" StartPoint=`"0,0`" EndPoint=`"0,1`"><GradientStop Offset=`"0`" Color=`"#38FFFFFF`" /><GradientStop Offset=`"1`" Color=`"#05FFFFFF`" /></LinearGradientBrush>")
  [void]$sb.AppendLine("            <LinearGradientBrush x:Key=`"WellShadeBrush`" StartPoint=`"0,0`" EndPoint=`"0,1`"><GradientStop Offset=`"0`" Color=`"$($c.WellShadow)`" /><GradientStop Offset=`"0.55`" Color=`"$($c.Clear)`" /></LinearGradientBrush>")
  [void]$sb.AppendLine("            <LinearGradientBrush x:Key=`"GlassSheenBrush`" StartPoint=`"0,0`" EndPoint=`"0.55,1`"><GradientStop Offset=`"0`" Color=`"$($c.GlassEdge)`" /><GradientStop Offset=`"0.38`" Color=`"$($c.Clear)`" /></LinearGradientBrush>")
  foreach ($k in $aliases.Keys) { [void]$sb.AppendLine("            <StaticResource x:Key=`"$k`" ResourceKey=`"$($aliases[$k])Brush`" />") }
  [void]$sb.AppendLine("        </ResourceDictionary>")
}
$static = Get-Content (Join-Path $PSScriptRoot 'Mangrove.static.xaml') -Raw
$out = $static.Replace('<!--THEMES-->', $sb.ToString().TrimEnd())
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'Mangrove.xaml'), $out, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Mangrove.xaml written"
