import io.pzstorm.storm.event.core.StormEventDispatcher;
import io.pzstorm.storm.event.core.SubscribeEvent;
import io.pzstorm.storm.event.lua.OnTickEvenPausedEvent;
import java.util.Random;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicLongArray;
import zombie.ai.astar.Mover;
import zombie.iso.IsoGridSquare;
import zombie.iso.IsoWorld;
import zombie.network.ServerMap;
import zombie.pathfind.IPathfinder;
import zombie.pathfind.Path;
import zombie.pathfind.nativeCode.PathfindNative;

/**
 * Path request generator for the downtown Louisville towers. Run it once on the server through
 * pzmcp's pz_java_eval. It needs no players online.
 *
 * <p>Each tick it keeps the cells around the towers loaded, loads and unloads a neighbouring ring
 * of cells, and submits path requests between random walkable squares on floors 0 to 23. Requests
 * whose path spanned 5 or more floors are remembered and replayed.
 *
 * <p>Counters live in the System property object "pfdrive.stats": 0 submitted, 1 ok, 2 failed,
 * 3 max nodes, 4 ok across 5+ floors, 5 errors, 6 ticks, 7 no square, 8 max floors, 9 replayed,
 * 10 remembered pairs.
 */
public class EvalScript implements IPathfinder {

    public static final AtomicLongArray S = new AtomicLongArray(16);
    static final Random R = new Random(42);
    static final EvalScript F = new EvalScript();
    static int id = 900000;
    static final int CX = 12740, CY = 1545;
    static java.lang.reflect.Method ADD;
    static final ConcurrentHashMap<Integer, float[]> PENDING = new ConcurrentHashMap<>();
    static final CopyOnWriteArrayList<float[]> LONG = new CopyOnWriteArrayList<>();
    static final long T0 = System.currentTimeMillis();

    public void Succeeded(Path p, Mover m) {
        S.incrementAndGet(1);
        float[] q = PENDING.remove(m.getID());
        int n = p.size();
        if (n > S.get(3)) S.set(3, n);
        float lo = 99, hi = -99;
        for (int i = 0; i < n; i++) {
            float z = p.getNode(i).z;
            lo = Math.min(lo, z);
            hi = Math.max(hi, z);
        }
        long dz = (long) (hi - lo);
        if (dz >= 5) {
            S.incrementAndGet(4);
            if (q != null && q[6] == 0 && LONG.size() < 400) {
                LONG.add(q);
                S.set(10, LONG.size());
            }
        }
        if (dz > S.get(8)) S.set(8, dz);
    }

    public void Failed(Mover m) {
        S.incrementAndGet(2);
        PENDING.remove(m.getID());
    }

    static IsoGridSquare pick(int z) {
        for (int i = 0; i < 30; i++) {
            int x = 12696 + R.nextInt(98), y = 1511 + R.nextInt(72);
            IsoGridSquare sq = IsoWorld.instance.currentCell.getGridSquare(x, y, z);
            if (sq != null && sq.getFloor() != null && !sq.isSolid() && !sq.isSolidTrans()) return sq;
        }
        S.incrementAndGet(7);
        return null;
    }

    static void submit(float[] q) throws Exception {
        final int my = id++;
        Mover m =
                (Mover)
                        java.lang.reflect.Proxy.newProxyInstance(
                                Mover.class.getClassLoader(),
                                new Class<?>[] {Mover.class},
                                (proxy, method, args) ->
                                        method.getName().equals("getID") || method.getName().equals("getPathFindIndex")
                                                ? (Object) my
                                                : method.getName().equals("hashCode")
                                                        ? (Object) my
                                                        : method.getName().equals("equals")
                                                                ? (Object) (proxy == args[0])
                                                                : "m" + my);
        PENDING.put(my, q);

        // addRequest gained a ninth parameter (avoidFarmingPlants) in 42.22.
        Object[] a = new Object[ADD.getParameterCount()];
        a[0] = F;
        a[1] = m;
        for (int i = 0; i < 6; i++) a[2 + i] = q[i];
        if (a.length == 9) a[8] = false;
        ADD.invoke(PathfindNative.instance, a);
        S.incrementAndGet(0);
    }

    @SubscribeEvent
    public static void onTick(OnTickEvenPausedEvent e) {
        try {
            S.incrementAndGet(6);
            int bx = CX / 64 - ServerMap.instance.getMinX(), by = CY / 64 - ServerMap.instance.getMinY();
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++) ServerMap.instance.loadOrKeepRelevent(bx + dx, by + dy);
            if (Integer.getInteger("pfdrive.churn", 1) == 1) {
                long phase = (System.currentTimeMillis() - T0) / 8000;
                if (phase % 2 == 0) {
                    int[][] off = {{2, 0}, {-2, 0}, {0, 2}, {0, -2}};
                    int[] o = off[(int) ((phase / 2) % 4)];
                    for (int dx = -1; dx <= 1; dx++)
                        for (int dy = -1; dy <= 1; dy++)
                            if (Math.abs(o[0] + dx) > 1 || Math.abs(o[1] + dy) > 1)
                                ServerMap.instance.loadOrKeepRelevent(bx + o[0] + dx, by + o[1] + dy);
                }
            }
            int rate = Integer.getInteger("pfdrive.rate", 20);
            if (S.get(0) - S.get(1) - S.get(2) > 300) return;
            for (int k = 0; k < rate; k++) {
                if (!LONG.isEmpty() && R.nextInt(10) < 7) {
                    float[] q = LONG.get(R.nextInt(LONG.size())).clone();
                    q[6] = 1;
                    submit(q);
                    S.incrementAndGet(9);
                    continue;
                }
                IsoGridSquare a = pick(R.nextInt(4) == 0 ? R.nextInt(24) : 0);
                IsoGridSquare b = pick(R.nextInt(24));
                if (a == null || b == null) continue;
                submit(
                        new float[] {
                            a.getX() + 0.5f, a.getY() + 0.5f, a.getZ(), b.getX() + 0.5f, b.getY() + 0.5f, b.getZ(), 0
                        });
            }
        } catch (Throwable t) {
            S.incrementAndGet(5);
            System.setProperty("pfdrive.err", String.valueOf(t) + " / " + String.valueOf(t.getCause()));
        }
    }

    public static Object run() {
        if (System.getProperties().get("pfdrive.stats") != null) return "already installed";
        for (java.lang.reflect.Method m : PathfindNative.class.getMethods())
            if (m.getName().equals("addRequest")) ADD = m;
        zombie.network.ServerOptions.instance.pauseEmpty.setValue(false);
        System.getProperties().put("pfdrive.stats", S);
        StormEventDispatcher.registerEventHandler(EvalScript.class);
        return "installed useNative=" + PathfindNative.useNativeCode + " addRequest params=" + ADD.getParameterCount();
    }
}
