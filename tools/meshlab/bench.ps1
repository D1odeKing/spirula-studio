<#
.SYNOPSIS
  Mesh-lab benchmark: every mesher variant on DTU, Tanks and Temples and ETH3D,
  scored by each benchmark's OFFICIAL evaluator. docs/meshlab.md, "Benchmarks".

.DESCRIPTION
  Per scene: (given poses, or -Sfm) -> spirula dense -> spirula geometry (normals)
  -> spirula train meshing -> each variant -> a PLY in the GT frame -> the
  official evaluator -> rows appended to results.csv
  (dataset,scene,variant,metric,value,commit,upstream_commit,protocol).

  Datasets and evaluator repositories are research-licensed and live OUTSIDE
  the repository, under -DataRoot:
    <DataRoot>/DTU/<scanN>                 2DGS-preprocessed COLMAP layout
    <DataRoot>/DTU_official/{Points,ObsMask}   the official DTU ground truth
    <DataRoot>/TNT/<Scene>                 COLMAP scene + <Scene>.ply, _COLMAP_SfM.log,
                                           .json crop, _trans.txt alignment
    <DataRoot>/ETH3D/<scene>               dslr_undistorted COLMAP layout + dslr_scan_eval
    <EvalRoot>/2dgs/scripts/eval_dtu       the DTU port 2DGS / GOF / PGSR ship
    <EvalRoot>/TanksAndTemples/python_toolbox/evaluation
    <EvalRoot>/multi-view-evaluation/build/ETH3DMultiViewEvaluation(.exe)

.EXAMPLE
  pwsh tools/meshlab/bench.ps1 -Dataset DTU -DataRoot D:\bench -EvalRoot D:\bench\eval -DryRun
#>
[CmdletBinding()]
param(
    [ValidateSet('DTU', 'TNT', 'ETH3D')] [string] $Dataset = 'DTU',
    [Parameter(Mandatory)] [string] $DataRoot,
    [string] $EvalRoot = (Join-Path $DataRoot 'eval'),
    [string[]] $Scenes,
    [ValidateSet('splat', 'splat+refine', 'dense', 'dense+refine')]
    [string[]] $Variants = @('splat', 'splat+refine', 'dense', 'dense+refine'),
    [string] $Spirula = (Join-Path $PSScriptRoot '..\..\build_vulkan\spirula.exe'),
    [string] $Python = 'python',
    [string] $Work = (Join-Path $DataRoot 'meshlab_runs'),
    [string] $Results = (Join-Path $DataRoot 'meshlab_runs\results.csv'),
    [string] $NormalModel = 'moge',
    [string] $DensePreset = 'balanced',
    [switch] $Sfm,          # Spirula's own SfM instead of the published poses; reported separately
    [switch] $Reuse,        # keep finished stages of an earlier run
    [switch] $DryRun        # print the commands, run nothing
)

$ErrorActionPreference = 'Stop'
$Repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')

$DefaultScenes = @{
    DTU   = @('scan24', 'scan37', 'scan40', 'scan55', 'scan63', 'scan65', 'scan69', 'scan83', 'scan97',
              'scan105', 'scan106', 'scan110', 'scan114', 'scan118', 'scan122')
    TNT   = @('Barn', 'Caterpillar', 'Courthouse', 'Ignatius', 'Meetingroom', 'Truck')
    ETH3D = @('courtyard', 'delivery_area', 'electro', 'facade', 'kicker', 'meadow', 'office', 'pipes',
              'playground', 'relief', 'relief_2', 'terrace', 'terrains')
}
if (-not $Scenes) { $Scenes = $DefaultScenes[$Dataset] }

function Git-Rev([string] $ref) {
    try { (git -C $Repo rev-parse --short $ref 2>$null).Trim() } catch { 'unknown' }
}
$Commit = Git-Rev 'HEAD'
$Upstream = Git-Rev 'upstream/master'
$PoseTag = if ($Sfm) { 'spirula-sfm' } else { 'given-poses' }

function Run([string] $exe, [string[]] $argv, [string] $log) {
    $line = "$exe " + ($argv -join ' ')
    Write-Host "  > $line"
    if ($DryRun) { return '' }
    $out = & $exe @argv 2>&1 | Tee-Object -FilePath $log
    if ($LASTEXITCODE -ne 0) { throw "failed ($LASTEXITCODE): $line  (log: $log)" }
    return ($out | Out-String)
}

function Done([string] $path) { return $Reuse -and (Test-Path $path) }

function Add-Result([string] $scene, [string] $variant, [string] $metric, $value, [string] $protocol) {
    if ($DryRun) { Write-Host "  = $scene $variant $metric (dry run)"; return }
    if (-not (Test-Path $Results)) {
        New-Item -ItemType Directory -Force (Split-Path $Results) | Out-Null
        'dataset,scene,variant,metric,value,commit,upstream_commit,protocol' | Set-Content $Results
    }
    "$Dataset,$scene,$variant,$metric,$value,$Commit,$Upstream,$protocol" | Add-Content $Results
    Write-Host "  = $scene $variant $metric $value"
}

# ---------------------------------------------------------------------------
# Reconstruction: one run per scene, shared by every variant
# ---------------------------------------------------------------------------
function Build-Scene([string] $scene, [string] $src, [string] $out) {
    New-Item -ItemType Directory -Force $out | Out-Null
    $data = $src
    if ($Sfm) {
        $data = Join-Path $out 'dataset'
        if (-not (Done (Join-Path $data 'sparse'))) {
            Run $Spirula @('sfm', 'auto', '--images', (Join-Path $src 'images'), '--output', $data) (Join-Path $out 'sfm.log') | Out-Null
        }
    }
    if (-not (Done (Join-Path $data 'dense\roma.ply'))) {
        Run $Spirula @('dense', $data, '--preset', $DensePreset) (Join-Path $out 'dense.log') | Out-Null
    }
    if (-not (Done (Join-Path $data 'normals'))) {
        Run $Spirula @('geometry', $data, '--model', $NormalModel) (Join-Path $out 'geometry.log') | Out-Null
    }
    $run = Join-Path $out 'run'
    if (-not (Done (Join-Path $run 'config.json'))) {
        Run $Spirula @('train', 'meshing', '--data', $data, '--output-dir-prefix', $out, '--output-dir-name', 'run',
                       '--keep-viewer-alive', '0') (Join-Path $out 'train.log') | Out-Null
    }
    return @{ data = $data; run = $run }
}

# Each variant's mesh in the dataset's FILE frame (what the official scripts
# take for these layouts; DTU's scale_mat is applied by its own evaluator).
function Mesh-Variant([string] $variant, $s, [string] $out) {
    $splat = Join-Path $out 'splat.ply'
    $dense = Join-Path $out 'dense.ply'
    switch ($variant) {
        'splat' {
            $raw = Join-Path $out 'splat_train.ply'
            if (-not (Done $splat)) {
                Run $Spirula @('mesh', $s.run, '--data', $s.data, '--output', $raw, '--format', 'ply') (Join-Path $out 'mesh.log') | Out-Null
                # The splat mesher writes the run's training frame.
                Run $Spirula @('mesh-eval', $raw, '--run', $s.run, '--dataset', $s.data, '--write-mesh', $splat) (Join-Path $out 'splat_frame.log') | Out-Null
            }
            return $splat
        }
        'splat+refine' {
            Mesh-Variant 'splat' $s $out | Out-Null
            $p = Join-Path $out 'splat_refined.ply'
            if (-not (Done $p)) {
                Run $Spirula @('mesh-refine', '--mesh', $splat, '--dataset', $s.data, '--anchor', 'dense', '-o', $p) (Join-Path $out 'refine_splat.log') | Out-Null
            }
            return $p
        }
        'dense' {
            if (-not (Done $dense)) {
                Run $Spirula @('mesh-dense', '--dataset', $s.data, '-o', $dense) (Join-Path $out 'mesh_dense.log') | Out-Null
            }
            return $dense
        }
        'dense+refine' {
            Mesh-Variant 'dense' $s $out | Out-Null
            $p = Join-Path $out 'dense_refined.ply'
            if (-not (Done $p)) {
                Run $Spirula @('mesh-refine', '--mesh', $dense, '--dataset', $s.data, '--anchor', 'dense', '-o', $p) (Join-Path $out 'refine_dense.log') | Out-Null
            }
            return $p
        }
    }
}

# ---------------------------------------------------------------------------
# Official evaluators
# ---------------------------------------------------------------------------
function Eval-DTU([string] $scene, [string] $mesh, [string] $out, [string] $variant) {
    $id = $scene -replace '^scan', ''
    $script = Join-Path $EvalRoot '2dgs\scripts\eval_dtu\evaluate_single_scene.py'
    $o = Join-Path $out "eval_$($variant -replace '\+', '_')"
    $text = Run $Python @($script, '--input_mesh', $mesh, '--scan_id', $id, '--output_dir', $o,
                         '--mask_dir', (Join-Path $DataRoot 'DTU'), '--DTU', (Join-Path $DataRoot 'DTU_official')) (Join-Path $out "eval_$id.log")
    if ($DryRun) { Add-Result $scene $variant 'chamfer_mm' '' "dtu-2dgs-port"; return }
    # The port prints "mean_d2s mean_s2d overall"; results.json is written beside it.
    $json = Join-Path $o 'results.json'
    if (Test-Path $json) {
        $r = Get-Content $json | ConvertFrom-Json
        Add-Result $scene $variant 'accuracy_mm' $r.mean_d2s 'dtu-2dgs-port'
        Add-Result $scene $variant 'completeness_mm' $r.mean_s2d 'dtu-2dgs-port'
        Add-Result $scene $variant 'chamfer_mm' $r.overall 'dtu-2dgs-port'
    } else {
        $n = [regex]::Matches($text, '[-+]?\d*\.\d+') | ForEach-Object { $_.Value }
        Add-Result $scene $variant 'chamfer_mm' $n[-1] 'dtu-2dgs-port'
    }
}

function Eval-TNT([string] $scene, [string] $mesh, [string] $out, [string] $variant) {
    $dir = Join-Path $EvalRoot 'TanksAndTemples\python_toolbox\evaluation'
    $gt = Join-Path $DataRoot "TNT\$scene"
    $o = Join-Path $out "eval_$($variant -replace '\+', '_')"
    $proto = 'tnt-python-toolbox@' + (git -C $dir rev-parse --short HEAD 2>$null)
    $text = Run $Python @((Join-Path $dir 'run.py'), '--dataset-dir', $gt, '--traj-path', (Join-Path $gt "$($scene)_COLMAP_SfM.log"),
                         '--ply-path', $mesh, '--out-dir', $o) (Join-Path $out "eval_tnt.log")
    if ($DryRun) { Add-Result $scene $variant 'f1' '' $proto; return }
    foreach ($m in @('precision', 'recall', 'f-score')) {
        $hit = [regex]::Match($text, "$m\s*[:=]?\s*([0-9.]+)", 'IgnoreCase')
        if ($hit.Success) { Add-Result $scene $variant ($m -replace 'f-score', 'f1') $hit.Groups[1].Value $proto }
    }
}

function Eval-ETH3D([string] $scene, [string] $mesh, [string] $out, [string] $variant) {
    # The official tool scores a point cloud: the mesh's area-uniform samples.
    $exe = Join-Path $EvalRoot 'multi-view-evaluation\build\ETH3DMultiViewEvaluation.exe'
    if (-not (Test-Path $exe)) { $exe = $exe -replace '\.exe$', '' }
    $pts = $mesh -replace '\.ply$', '_samples.ply'
    Run $Spirula @('mesh-eval', $mesh, '--write-samples', $pts, '--samples', '5000000') (Join-Path $out 'samples.log') | Out-Null
    $mlp = Join-Path $DataRoot "ETH3D\$scene\dslr_scan_eval\scan_alignment.mlp"
    $text = Run $exe @('--reconstruction_ply_path', $pts, '--ground_truth_mlp_path', $mlp, '--tolerances', '0.01,0.02,0.05,0.1') (Join-Path $out 'eval_eth3d.log')
    if ($DryRun) { Add-Result $scene $variant 'f1@0.02' '' 'eth3d-mve'; return }
    $tol = @('0.01', '0.02', '0.05', '0.1')
    foreach ($kind in @('Completeness', 'Accuracy', 'F1-scores')) {
        $hit = [regex]::Match($text, "$($kind):\s*([0-9. ]+)")
        if (-not $hit.Success) { continue }
        $vals = $hit.Groups[1].Value.Trim() -split '\s+'
        for ($i = 0; $i -lt [Math]::Min($vals.Count, $tol.Count); $i++) {
            Add-Result $scene $variant "$(($kind -replace '-scores', '').ToLower())@$($tol[$i])" $vals[$i] 'eth3d-mve'
        }
    }
}

# ---------------------------------------------------------------------------
foreach ($scene in $Scenes) {
    Write-Host "== $Dataset / $scene ($PoseTag)"
    $src = Join-Path $DataRoot "$Dataset\$scene"
    if (-not $DryRun -and -not (Test-Path $src)) { Write-Warning "missing $src"; continue }
    $out = Join-Path $Work "$Dataset\$scene\$PoseTag"
    try {
        $s = Build-Scene $scene $src $out
        foreach ($v in $Variants) {
            $mesh = Mesh-Variant $v $s $out
            switch ($Dataset) {
                'DTU'   { Eval-DTU $scene $mesh $out ($v + "/" + $PoseTag) }
                'TNT'   { Eval-TNT $scene $mesh $out ($v + "/" + $PoseTag) }
                'ETH3D' { Eval-ETH3D $scene $mesh $out ($v + "/" + $PoseTag) }
            }
        }
    } catch {
        Write-Warning "$scene failed: $_"
    }
}
