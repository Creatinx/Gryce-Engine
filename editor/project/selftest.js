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
let cube = 0;
let ground = 0;
let lightHash = '';

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
    check('view_info.uiBackend', view.uiBackend === 'builtin',
          'uiBackend=' + view.uiBackend);
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
        // 界面：宿主必须真的把回调跑起来。只看面板 JSON 有数据是不够的 ——
        // 那证明不了"引擎在画界面"，也证明不了绘制没崩。
        check('ui.available', E.ui_available() === true);
        check('ui.frames', E.ui_frameCount() > 0, 'frames=' + E.ui_frameCount());

        const ui = JSON.parse(E.ui_state());
        check('ui.state', ui.ok === 1 && ui.enabled === true &&
                          ui.uiReady === true && ui.uiBackend === 'builtin',
              'enabled=' + ui.enabled + ' backend=' + ui.uiBackend +
              ' ready=' + ui.uiReady);
        check('ui.drew', ui.draws > 0, 'draws=' + ui.draws);
        // 视口矩形由绘制侧写回，tick 据此设渲染目标尺寸；为 0 说明界面
        // 从没画出过视口图片（贴图 id 拿不到或布局塌了）。
        check('ui.viewport', ui.viewWidth > 0 && ui.viewHeight > 0,
              ui.viewWidth + 'x' + ui.viewHeight);
        check('ui.viewportTex', ui.viewportTex !== 0, 'tex=' + ui.viewportTex);
        check('ui.pointerInView', typeof ui.pointerInView === 'boolean');
        phase = 11;
        return;
    }

    if (phase === 11) {
        // 模型层：UI 与 JSON 读同一份数据，校验界面依赖的字段都在。
        const tree = JSON.parse(E.panel_hierarchy());
        check('ui.hierarchy.depth', tree.roots.length > 0 && tree.roots[0].depth === 0 &&
              typeof tree.roots[0].children === 'object');
        const assets = JSON.parse(E.panel_assets('res:/models'));
        // count 与 items 必须一致：两者不是同一次遍历算出来的，对不上说明
        // 模型和序列化走了岔路。
        check('ui.assets.count', assets.count === assets.items.length && assets.count >= 1,
              'count=' + assets.count + ' items=' + assets.items.length);
        check('ui.inspector', JSON.parse(E.panel_inspector(handleOf('Cube'))).ok === 1);
        phase = 12;
        return;
    }

    if (phase === 12) {
        // 组件增删的"命令面"：加一个 Light 到 Cube 上。
        cube = handleOf('Cube');
        ground = handleOf('Ground');
        const ins = JSON.parse(E.panel_inspector(cube));
        check('comp.inspector', ins.ok === 1 && ins.components.length > 0,
              'components=' + (ins.components ? ins.components.length : -1));

        // 引擎内部维护的类型不得出现在 Add 菜单里（挂了没有意义，
        // Transform 挂两个更是灾难）。Cube 上并没有 Transform 组件 ——
        // 位置存在 Entity 自身，反射表里那条注册只服务于反序列化。
        const mesh = ins.components.find(function (c) { return c.name === 'MeshRenderer'; });
        check('comp.removable', mesh !== undefined && mesh.removable === true,
              mesh ? 'removable=' + mesh.removable : 'no MeshRenderer');
        // 哈希必须是可精确往返的字符串：64 位值经 JSON 数字只剩 53 位精度，
        // 读回去是个不同的数，表现是"类型查得到、属性写不进去"。
        check('comp.hashIsString', mesh !== undefined && typeof mesh.hash === 'string',
              mesh ? typeof mesh.hash : 'no MeshRenderer');

        const availNames = ins.availableComponents.map(function (c) { return c.shortName; });
        check('comp.available', availNames.length > 0, 'available=' + availNames.length);
        check('comp.internalExcluded',
              availNames.indexOf('Transform') < 0 &&
              availNames.indexOf('ParentComponent') < 0 &&
              availNames.indexOf('ChildrenComponent') < 0,
              'internal bookkeeping types must not be addable');

        check('comp.add', E.addComponent(cube, 'Light') === true);
        phase = 13;
        return;
    }

    if (phase === 13) {
        const ins = JSON.parse(E.panel_inspector(cube));
        const names = ins.components.map(function (c) { return c.name; });
        check('comp.added', names.indexOf('Light') >= 0, names.join(','));
        // 加进去的类型必须同时从候选里消失，否则菜单会允许重复添加。
        const avail = ins.availableComponents.map(function (c) { return c.shortName; });
        check('comp.addedRemovedFromMenu', avail.indexOf('Light') < 0,
              'available=' + avail.length);

        // 向量属性写入：Light.direction 是 vec3，有确定答案。
        const light = ins.components.find(function (c) { return c.name === 'Light'; });
        check('comp.lightFound', light !== undefined);
        lightHash = light ? light.hash : '';
        check('prop.vec',
              E.setPropertyVec(cube, lightHash, 'direction', 0.25, -1, 0.5) === true);
        phase = 14;
        return;
    }

    if (phase === 14) {
        const ins = JSON.parse(E.panel_inspector(cube));
        const light = ins.components.find(function (c) { return c.name === 'Light'; });
        let dir = null;
        if (light) {
            for (let i = 0; i < light.properties.length; ++i) {
                if (light.properties[i].name === 'direction') dir = light.properties[i].value;
            }
        }
        check('prop.vecApplied',
              dir !== null && near(dir[0], 0.25) && near(dir[1], -1) && near(dir[2], 0.5),
              dir ? dir.join(',') : 'null');
        // 分量数与字段类型不符时必须拒绝，不能写一半。
        check('prop.vecReject',
              E.setPropertyVec(cube, lightHash, 'direction', 1, 2) === false);

        // 重挂：把 Cube 挂到 Ground 下。
        check('reparent', E.reparent(cube, ground) === true);
        phase = 15;
        return;
    }

    if (phase === 15) {
        const ins = JSON.parse(E.panel_inspector(cube));
        check('reparent.applied', ins.parent === ground, 'parent=' + ins.parent);

        // 成环必须被拒：把父节点挂到自己的子节点下会让场景不可遍历。
        check('reparent.cycleReject', E.reparent(ground, cube) === false);
        check('reparent.selfReject', E.reparent(cube, cube) === false);

        // 组件删除：删掉刚才加的 Light，它应当回到候选里。
        check('comp.remove', E.removeComponent(cube, lightHash) === true);
        phase = 16;
        return;
    }

    if (phase === 16) {
        const ins = JSON.parse(E.panel_inspector(cube));
        const names = ins.components.map(function (c) { return c.name; });
        check('comp.removed', names.indexOf('Light') < 0, names.join(','));
        const avail = ins.availableComponents.map(function (c) { return c.shortName; });
        check('comp.backInMenu', avail.indexOf('Light') >= 0, 'available=' + avail.length);
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
