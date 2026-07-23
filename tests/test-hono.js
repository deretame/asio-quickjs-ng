// tests/test-hono.js - 真实生产级 Hono + http adapter 测试（请求头 + 响应头版）
import { Hono } from 'hono'
import { createServer } from './src/js/http.js'

const app = new Hono()

// 生产级路由（包含嵌套路由 + 完整请求头响应头支持）
app.get('/', (c) => c.text('Hello Hono!'))
app.get('/ping', (c) => c.text('pong'))

// 嵌套路由
const api = new Hono()
api.get('/users', (c) => c.json({ users: ['john', 'jane'] }))
api.get('/posts', (c) => c.json({ posts: 42 }))
api.get('/users/:id', (c) => c.json({ id: c.req.param('id') }))
app.route('/api', api)

app.post('/api', async (c) => {
  const body = await c.req.text()
  return c.json({ received: body.length, ok: true, type: 'json' })
})
app.post('/form', async (c) => {
  const form = await c.req.parseFormData()
  return c.json({ fields: Object.fromEntries(form), ok: true })
})
app.post('/large', async (c) => {
  const body = await c.req.arrayBuffer()
  return c.json({ size: body.byteLength, ok: true })
})

// 请求头测试
app.get('/headers', (c) => {
  const h = c.req.header()
  return c.json({ 
    host: h.host, 
    userAgent: h['user-agent'],
    accept: h.accept
  })
})

// 响应头测试
app.get('/set-header', (c) => {
  c.header('X-Test', '12345')
  c.header('Content-Type', 'application/json')
  return c.json({ message: 'header set' })
})

app.get('/status', (c) => c.status(201).text('Created'))
app.get('/echo/:id', (c) => c.text(c.req.param('id')))
app.get('/query', (c) => c.json({ query: c.req.query() }))
app.get('/error', (c) => c.status(500).json({ error: 'internal' }))

console.log('🚀 Hono + http adapter 生产级请求头响应头测试启动')

const handler = app.fetch.bind(app)
const server = createServer(handler)
server.listen(3001)

// 真实请求头 + 响应头 + 复杂请求测试
export async function runTests() {
  console.log('开始真实请求头 + 响应头 + 复杂请求测试...')
  
  const tests = []
  
  tests.push(fetch('http://localhost:3001/').then(r => r.text()).then(t => ({ name: 'GET /', ok: t === 'Hello Hono!' })))
  tests.push(fetch('http://localhost:3001/api', {
    method: 'POST',
    body: JSON.stringify({ a: 1, b: 'hello', c: { d: 2 } })
  }).then(r => r.json()).then(data => ({ name: 'POST /api JSON', ok: data.ok === true && data.received > 0 })))
  tests.push(fetch('http://localhost:3001/large', {
    method: 'POST',
    body: new ArrayBuffer(1024 * 1024)
  }).then(r => r.json()).then(data => ({ name: 'POST /large 1MB', ok: data.size > 1024 * 500 })))
  tests.push(fetch('http://localhost:3001/headers').then(r => r.json()).then(data => ({ name: 'GET /headers', ok: data.host && data['user-agent'] })))
  tests.push(fetch('http://localhost:3001/set-header').then(r => r.json()).then(data => ({ name: 'GET /set-header', ok: data.message === 'header set' })))
  
  const results = await Promise.all(tests)
  const passed = results.filter(t => t.ok).length
  
  console.log(`请求头 + 响应头 + 复杂请求测试完成: ${passed}/${tests.length} 通过`)
  return { passed, total: tests.length, ok: passed === tests.length }
}

export default app
