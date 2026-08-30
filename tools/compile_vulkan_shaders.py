#!/usr/bin/env python3
"""
Vulkan Shader Compiler for Gryce Engine
Compiles GLSL shaders to SPIR-V binary format (.spv)

Usage:
    python compile_vulkan_shaders.py [shader_dir]

Arguments:
    shader_dir: Directory containing GLSL shaders (default: examples/common/shaders)

Requirements:
    - Vulkan SDK installed (glslc or glslangValidator in PATH)
"""

import os
import sys
import subprocess
import argparse
from pathlib import Path
from typing import List, Tuple


def find_compiler() -> str:
    """Find available GLSL to SPIR-V compiler"""
    for compiler in ['glslc', 'glslangValidator']:
        try:
            result = subprocess.run([compiler, '--version'], 
                                  capture_output=True, 
                                  text=True,
                                  timeout=5)
            if result.returncode == 0:
                print(f"Found compiler: {compiler}")
                return compiler
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue
    
    raise RuntimeError("No GLSL to SPIR-V compiler found. Install Vulkan SDK (glslc or glslangValidator)")


def compile_shader(compiler: str, input_path: Path, output_path: Path) -> bool:
    """Compile a single shader file to SPIR-V"""
    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        
        if compiler == 'glslc':
            cmd = [compiler, str(input_path), '-o', str(output_path)]
        else:  # glslangValidator
            cmd = [compiler, '-V', str(input_path), '-o', str(output_path)]
        
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        
        if result.returncode != 0:
            print(f"  ERROR compiling {input_path.name}:")
            print(f"    {result.stderr}")
            return False
        
        return True
        
    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT compiling {input_path.name}")
        return False
    except Exception as e:
        print(f"  EXCEPTION compiling {input_path.name}: {e}")
        return False


def find_vulkan_shaders(shader_dir: Path) -> List[Tuple[Path, str]]:
    """Find all Vulkan shader files (vulkan_*.vert, vulkan_*.frag)"""
    shaders = []
    
    for file_path in shader_dir.rglob('vulkan_*.vert'):
        shaders.append((file_path, 'vert'))
    
    for file_path in shader_dir.rglob('vulkan_*.frag'):
        shaders.append((file_path, 'frag'))
    
    return shaders


def compile_all_shaders(shader_dir: Path, output_dir: Path) -> Tuple[int, int]:
    """Compile all Vulkan shaders to SPIR-V"""
    compiler = find_compiler()
    shaders = find_vulkan_shaders(shader_dir)
    
    if not shaders:
        print(f"No Vulkan shaders found in {shader_dir}")
        return 0, 0
    
    print(f"\nCompiling {len(shaders)} shaders...")
    print(f"Source: {shader_dir}")
    print(f"Output: {output_dir}\n")
    
    success_count = 0
    fail_count = 0
    
    for input_path, stage in shaders:
        # Create output filename: vulkan_pbr.vert -> vulkan_pbr.vert.spv
        output_filename = f"{input_path.name}.spv"
        output_path = output_dir / output_filename
        
        print(f"  [{stage}] {input_path.name} -> {output_filename}")
        
        if compile_shader(compiler, input_path, output_path):
            success_count += 1
        else:
            fail_count += 1
    
    return success_count, fail_count


def main():
    parser = argparse.ArgumentParser(description='Compile GLSL shaders to SPIR-V for Vulkan')
    parser.add_argument('shader_dir', nargs='?', 
                       default='examples/common/shaders',
                       help='Directory containing GLSL shaders')
    parser.add_argument('--output', '-o', 
                       default=None,
                       help='Output directory for SPIR-V files (default: <shader_dir>/spirv)')
    
    args = parser.parse_args()
    
    shader_dir = Path(args.shader_dir).resolve()
    if not shader_dir.exists():
        print(f"ERROR: Shader directory not found: {shader_dir}")
        sys.exit(1)
    
    output_dir = Path(args.output) if args.output else shader_dir / 'spirv'
    output_dir = output_dir.resolve()
    
    try:
        success, fail = compile_all_shaders(shader_dir, output_dir)
        
        print(f"\n{'='*60}")
        print(f"Compilation complete:")
        print(f"  Success: {success}")
        print(f"  Failed:  {fail}")
        print(f"  Total:   {success + fail}")
        print(f"{'='*60}\n")
        
        if fail > 0:
            sys.exit(1)
            
    except Exception as e:
        print(f"\nERROR: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
