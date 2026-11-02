import type { EngineInterface, Register, SessionRateLimit } from 'claude-code'

// 把 Claude 订阅额度的窗口（five_hour / seven_day）写进一个缓存文件，给桌面小面板读
// （esp32-26/deck 的 AI QUOTA）。额度本来就随每次 API 响应回来，窗口挪动一个整点时引擎推
// session.measure，这里只是顺手记下来：不跑 `claude -p /usage`，也不多发任何请求。
// 几个会话同时开着时谁最后写谁的，反正是同一个账号的同一组数。

const CACHE = 'Library/Caches/deck/claude-limits.json'

async function save($: EngineInterface, limits: SessionRateLimit[]): Promise<void> {
  if (limits.length === 0) return  // 不是订阅账号，或者这个会话还没收到第一次响应
  const home = await $.env.get('HOME')
  if (!home) return
  const body = {
    v: 1,
    at: await $.clock.now(),
    limits: limits.map(l => ({ kind: l.kind, used: l.percentUsed, resets_at: l.resetsAt ?? null })),
  }
  await $.fs.write(`${home}/${CACHE}`, JSON.stringify(body) + '\n')
}

export const register: Register = on => {
  // 恢复的会话一开始就有上次的读数
  on('session.start', async ($, e, next) => {
    const started = await next(e)
    await save($, (await $.session.usage()).rateLimits).catch(() => undefined)
    return started
  })

  on('session.measure', async ($, e, next) => {
    const measured = await next(e)
    if (e.changed.includes('rateLimits')) {
      await save($, e.rateLimits).catch(() => undefined)
    }
    return measured
  })
}
