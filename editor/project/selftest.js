// ===========================================================================
// editor/project/selftest.js — 编辑器接口自检
//
// 不替代 unit test，补的是单测覆盖不到的那一段：扩展真的被宿主加载了、
// JS 桥真的把参数传对了、命令真的在下一帧生效了。
//
// 跑法：
//   build/GryceApp.exe --project editor/project --entry res:/selftest.js --frames 40
//
// 结论只有两行之一："SELFTEST PASS n/n" 或 "SELFTEST FAIL n/m"，
// CI 里 grep 这一行即可（宿主退出码不反映脚本结论）。
//
// 三条写法约束，都是为了在这套 JS 引擎上少踩坑：
//   1. 带返回值的接口一律回 JS 布尔（true/false），不是 1/0 —— 断言用真值。
//   2. 不用 switch + const：QuickJS 解析 case 子句里的块级声明会报
//      "unexpected token: '}'"，且错误位置指向几十行之前，极难排查。
//   3. 断言按帧推进：GEdit_* 入队后要等下一帧 GCore_BeginFrame 才执行，
//      同一帧读回必然是旧值。
// ===========================================================================

const W = 1280;
const H = 720;

let E = null;
let phase = 1;
let frame = 0;
let checks = 0;
let failures = 0;
let created = 0;
let entitiesBefore = 0;
let savedPos = [0, 0, 0];

function check(name, ok, detail) {
    ++checks;
    if (!ok) ++failures;
    let tail = '';
    if (detail !== undefined && detail !== null && detail !== '') tail = ' :: ' + detail;
    engine.log.info((ok ? 'PASS ' : 'FAIL ') + name + tail);
}

function near(a, b) {
    return Math.abs(a - b) < 0.001;
}

function findHandle(node, name) {
    if (node.name === name) return node.handle;
    const kids = node.children || [];
    for (let i = 0; i < kids.length; ++i) {
        const hit = findHandle(kids[i], name);
        if (hit > 0) return hit;
    }
    return 0;
}

function handleOf(name) {
    const tree = JSON.parse(E.panel_hierarchy());
    for (let i = 0; i < tree.roots.length; ++i) {
        const hit = findHandle(tree.roots[i], name);
        if (hit > 0) return hit;
    }
    return 0;
}

function localPos(handle) {
    const ins = JSON.parse(E.panel_inspector(handle));
    if (!ins || !ins.ok) return null;
    return ins.transform.position;
}

export function on_start() {
    engine.log.info('selftest: start');

    if (!engine.window.create({ title: 'Gryce Editor Selftest', width: W, height: H })) {
        engine.log.error('SELFTEST FAIL: window.create');
        engine.app.quit();
        return;
    }
    if (!engine.scene.load('res:/scenes/main.gesc')) {
        engine.log.error('SELFTEST FAIL: scene load');
        engine.app.quit();
        return;
    }
    if (typeof engine.editor === 'undefined' || !engine.editor) {
        engine.log.error('SELFTEST FAIL: engine.editor missing (build GryceEditorExtension)');
        engine.app.quit();
        return;
    }
    E = engine.editor;

    // --- 扩展身份 -----------------------------------------------------------
    check('version', E.version().indexOf('editor-ext') === 0, E.version());

    // --- 视口 ---------------------------------------------------------------
    E.view_enableOutput(true);
    check('view_setSize', E.view_setSize(W, H) === true);
    E.view_setEditorCamera(0, 2.2, 8, 0, -10, 55);

    const view = JSON.parse(E.view_info());
    check('view_info.size', view.width === W && view.height === H,
          view.width + 'x' + view.height);
    check('view_info.output', view.output === true, 'output=' + view.output);
    check('view_info.hasImGui', typeof view.hasImGui === 'boolean',
          'hasImGui=' + view.hasImGui);
    // 线框支持必须在初始化阶段就有确定答案，不能等第一次切换才知道
    check('displayModeSupported', typeof view.wireframeSupported === 'boolean',
          'wireframeSupported=' + view.wireframeSupported);

    // --- Gizmo --------------------------------------------------------------
    check('gizmo_enable', E.gizmo_enable(true) === true);
    check('gizmo_setMode', E.gizmo_setMode(0, 0) === true);
    const gm = JSON.parse(E.gizmo_mode());
    check('gizmo_mode', gm.enabled === true && gm.operation === 0 && gm.space === 0,
          'op=' + gm.operation + ' space=' + gm.space);
    check('gizmo_setMode.reject', E.gizmo_setMode(9, 0) === false,
          'out-of-range operation');

    // --- 层级 / 检视 --------------------------------------------------------
    const tree = JSON.parse(E.panel_hierarchy());
    check('hierarchy.count', tree.ok === 1 && tree.count === 7, 'count=' + tree.count);
    // MainCamera / Sun / Cube / Ground / Props 是根，CrateA/B 挂在 Props 下
    check('hierarchy.roots', tree.roots.length === 5, 'roots=' + tree.roots.length);

    const cube = handleOf('Cube');
    check('hierarchy.find', cube > 0, 'Cube handle=' + cube);

    const ins = JSON.parse(E.panel_inspector(cube));
    check('inspector.ok', ins.ok === 1 && ins.name === 'Cube', ins.name);
    check('inspector.components', ins.components.length >= 1,
          'components=' + ins.components.length);

    let meshValue = null;
    for (let c = 0; c < ins.components.length; ++c) {
        const comp = ins.components[c];
        if (comp.name !== 'MeshRenderer') continue;
        for (let p = 0; p < comp.properties.length; ++p) {
            if (comp.properties[p].name === 'mesh_path') meshValue = comp.properties[p].value;
        }
    }
    check('inspector.property', meshValue === 'res:/models/cube.obj', 'mesh_path=' + meshValue);

    // --- 资产 ---------------------------------------------------------------
    const scan = JSON.parse(E.asset_scan('res:/models', 0));
    check('asset_scan.count', scan.count >= 1, 'count=' + scan.count);
    check('asset_scan.kind', scan.items.length > 0 && scan.items[0].kind === 'mesh',
          scan.items.length > 0 ? scan.items[0].name : '');
    // 侧车是资产身份的来源。cube.obj 没有 .meta 时 has_meta=0，
    // 这是合法状态（只有显式导入才建侧车），所以只校验字段存在。
    check('asset_scan.fields', scan.items.length > 0
          && typeof scan.items[0].has_meta === 'number'
          && typeof scan.items[0].guid === 'string');

    const meta = JSON.parse(E.asset_meta('res:/models/cube.obj'));
    check('asset_meta.shape', typeof meta.ok === 'number'
          && (meta.ok === 0 || (typeof meta.guid === 'string' && meta.guid.length === 32)),
          'ok=' + meta.ok);

    const panelAssets = JSON.parse(E.panel_assets('res:/models'));
    check('panel_assets', panelAssets.ok === 1 && panelAssets.listing.length > 16);

    // --- 选择 ---------------------------------------------------------------
    check('select', E.select(cube) === true);
    // 非法参数有两种拒绝形态：语义非法返回 false，类型完全不对抛错。
    // 两种都算"拒绝了"，这里都接受 —— 断言的是"不会静默成功"。
    let rejected = false;
    try {
        rejected = E.select(-5) === false;
    } catch (e) {
        rejected = true;
    }
    check('select.reject', rejected, 'negative handle');
    const sel = JSON.parse(E.selection());
    check('selection.primary', sel.primary === cube, 'primary=' + sel.primary);

    // --- 快捷键 -------------------------------------------------------------
    check('shortcut_bind', E.shortcut_bind('selftest.noop', 348, 0) === true);
    check('shortcut_conflict', E.shortcut_bind('selftest.other', 348, 0) === false,
          'same combo twice must be rejected');
    check('shortcut_poll', E.shortcut_poll() === '[]', 'no keys pressed');

    // --- 初始状态 -----------------------------------------------------------
    const stats = JSON.parse(E.panel_stats());
    entitiesBefore = stats.entities;
    check('stats.entities', stats.entities === 7, 'entities=' + stats.entities);
    check('history.empty', JSON.parse(E.history()).canUndo === false);

    phase = 2;
}

export function on_update(dt) {
    if (!E) return;
    ++frame;

    // 每帧都要 tick：pending 创建在这里确认，面板缓存也在这里失效。
    const events = E.tick(dt);
    if (events.indexOf('created:') >= 0) {
        const list = JSON.parse(events);
        for (let i = 0; i < list.length; ++i) {
            if (list[i].indexOf('created:') === 0) {
                created = parseInt(list[i].substring(8), 10);
            }
        }
    }

    if (phase === 2) {
        check('create', E.create('SelftestEntity', 0) === true);
        phase = 3;
        return;
    }

    if (phase === 3) {
        // 等确认。GEdit_CreateEntity 只入队，句柄靠"创建即选中"回读。
        if (created > 0) {
            check('create.handle', created > 0, 'handle=' + created);
            check('create.autoSelect', JSON.parse(E.selection()).primary === created);
            check('create.count',
                  JSON.parse(E.panel_stats()).entities === entitiesBefore + 1,
                  'entities=' + JSON.parse(E.panel_stats()).entities);
            const p = localPos(created);
            savedPos = p ? [p[0], p[1], p[2]] : [0, 0, 0];
            check('create.transform', p !== null, p ? p.join(',') : 'null');
            phase = 4;
        } else if (frame > 16) {
            check('create.handle', false, 'no created event within 16 frames');
            finish();
        }
        return;
    }

    if (phase === 4) {
        // setPosition 是绝对值，translate 是增量，两条独立命令各占一步撤销
        check('setPosition', E.setPosition(created, 1, 2, 3) === true);
        check('translate', E.translate(created, 0.5, 0, 0) === true);
        check('setPosition.reject', E.setPosition(created, 0 / 0, 0, 0) === false,
              'NaN must be rejected');
        check('setScale.reject', E.setScale(created, 0, 1, 1) === false,
              'zero scale must be rejected');
        phase = 5;
        return;
    }

    if (phase === 5) {
        const p = localPos(created);
        check('transform.applied',
              p !== null && near(p[0], 1.5) && near(p[1], 2) && near(p[2], 3),
              p ? p.join(',') : 'null');
        const hist = JSON.parse(E.history());
        check('history.grew', hist.canUndo === true && hist.undo >= 2, 'undo=' + hist.undo);
        check('session.dirty', JSON.parse(E.session()).dirty === true);
        check('undo.transform', E.undo() === true);
        phase = 6;
        return;
    }

    if (phase === 6) {
        check('undo.transform2', E.undo() === true);
        phase = 7;
        return;
    }

    if (phase === 7) {
        const p = localPos(created);
        check('undo.restored',
              p !== null && near(p[0], savedPos[0]) && near(p[1], savedPos[1])
              && near(p[2], savedPos[2]),
              p ? p.join(',') : 'null');
        check('redo.available', JSON.parse(E.history()).canRedo === true);
        check('redo', E.redo() === true);
        phase = 8;
        return;
    }

    if (phase === 8) {
        const p = localPos(created);
        check('redo.applied', p !== null && near(p[0], 1), p ? p.join(',') : 'null');
        check('destroy', E.destroy(created) === true);
        phase = 9;
        return;
    }

    if (phase === 9) {
        check('destroy.count',
              JSON.parse(E.panel_stats()).entities === entitiesBefore,
              'entities=' + JSON.parse(E.panel_stats()).entities);
        // 拾取：相机在 (0,2.2,8) 看 -Z，命中与否都算通过 ——
        // 这里验证的是"射线能算出来且不崩"，不是命中率。
        const hit = E.pick(W * 0.5, H * 0.55);
        check('pick.ran', hit >= 0, 'hit=' + hit);
        phase = 10;
        return;
    }

    if (phase === 10) {
        finish();
    }
}

function finish() {
    if (failures === 0) {
        engine.log.info('SELFTEST PASS ' + checks + '/' + checks);
    } else {
        engine.log.error('SELFTEST FAIL ' + (checks - failures) + '/' + checks);
    }
    E.close();
    engine.app.quit();
}

export function on_destroy() {
    engine.log.info('selftest: done');
}
