p = r'D:/Gryce-Engine/core/render/render_context.cpp'
with open(p, 'r', encoding='utf-8') as f:
    lines = f.readlines()

# Find and remove the duplicate block at lines 570-575 (0-indexed: 569-574)
# The duplicate starts with "    if (!cmd_buffer_) return;" after "process_pending_destroys(true);"
i = 0
while i < len(lines):
    if lines[i].strip() == 'process_pending_destroys(true);':
        # Check if the next lines form the duplicate present() body
        j = i + 1
        if j < len(lines) and lines[j].strip() == '}':
            # Skip past the closing brace of present_sync()
            j += 1
            # Now check for duplicate
            duplicate = [
                '    if (!cmd_buffer_) return;',
                '    cmd_buffer_->submit();',
                '    // 等待渲染线程完成本帧所有已提交命令，避免 CPU 侧在 GPU 仍在引用资源时释放实体/材质。',
                '    wait_for_idle();',
                '    process_pending_destroys();',
                '}'
            ]
            match = True
            for k, expected in enumerate(duplicate):
                if j + k >= len(lines) or lines[j + k].strip() != expected:
                    match = False
                    break
            if match:
                # Remove the duplicate block
                del lines[j:j + len(duplicate)]
                print(f'Removed duplicate present() body at line {j + 1}')
                break
    i += 1

with open(p, 'w', encoding='utf-8') as f:
    f.writelines(lines)
print('Fixed render_context.cpp')
