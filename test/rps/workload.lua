-- Fixed weighted requests, no HTTP pipelining. Validate every response body.
threads = {}

function setup(thread)
  table.insert(threads, thread)
end

function init(args)
  paths, requests, sent, bodies = {}, {}, {}, {}
  invalid, responses, sequence, refreshes = 0, 0, 0, 0
  steady_responses, steady_invalid = 0, 0
  measure_from = tonumber(os.getenv("BENCH_MEASURE_FROM_US"))
  measure_to = tonumber(os.getenv("BENCH_MEASURE_TO_US"))
  json_mode = os.getenv("BENCH_JSON") == "1"
  lsn = os.getenv("BENCH_LSN") or ""
  lsn_file = os.getenv("BENCH_LSN_FILE")
  allow_hosts = false
  for line in io.lines(os.getenv("BENCH_PROFILE")) do
    local weight, path = line:match("^%s*(%d+)%s+(%S+)")
    if weight then
      for i = 1, tonumber(weight) do
        table.insert(paths, path)
      end
      if path == "/hosts" then
        allow_hosts = true
      end
    end
  end
  assert(#paths > 0, "empty workload")
  if json_mode then
    wrk.headers["Accept"] = "application/json"
  end
  if os.getenv("BENCH_CLOSE") == "1" then
    wrk.headers["Connection"] = "close"
  end
  rebuild()
end

function rebuild()
  for i, path in ipairs(paths) do
    local expanded = path:gsub("{lsn}", lsn)
    requests[i] = wrk.format("GET", expanded)
  end
end

function request()
  if lsn_file and sequence % 128 == 0 then
    local file = assert(io.open(lsn_file, "r"))
    local next_lsn = file:read("*l")
    file:close()
    assert(next_lsn and next_lsn:match("^%x+/%x+$"), "invalid LSN")
    if next_lsn ~= lsn then
      lsn = next_lsn
      rebuild()
      refreshes = refreshes + 1
    end
  end
  local i = sequence % #requests + 1
  sequence = sequence + 1
  sent[paths[i]] = (sent[paths[i]] or 0) + 1
  return requests[i]
end

function invalidate()
  invalid = invalid + 1
  if within_window then
    steady_invalid = steady_invalid + 1
  end
end

function response(status, headers, body)
  responses = responses + 1
  local now = wrk.time_us()
  within_window = now >= measure_from and now < measure_to
  if within_window then
    steady_responses = steady_responses + 1
  end
  if status ~= 200 then
    invalidate()
    return
  end
  if body:sub(1, 1) == "[" and allow_hosts then
    local seen, count, alive = {}, 0, 0
    for host in body:gmatch('"host":"([^"]+)"') do
      if seen[host] or not host:match("^127%.0%.0%.[123]$") then
        invalidate()
        return
      end
      seen[host] = true
      count = count + 1
    end
    for _ in body:gmatch('"alive":true') do
      alive = alive + 1
    end
    if count ~= 3 or alive ~= 3 then
      invalidate()
    end
    count_hosts_json = (count_hosts_json or 0) + 1
    return
  end
  local host = body
  if json_mode then
    host = body:match('^%{"host":"([^"]+)","dc":.-,"geo":.-%}$')
  end
  if not host or not host:match("^127%.0%.0%.[123]$") then
    invalidate()
    return
  end
  _G["count_" .. host] = (_G["count_" .. host] or 0) + 1
end

local function encode(value)
  if type(value) == "number" then
    return tostring(value)
  end
  if type(value) == "string" then
    return '"'
      .. value:gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n')
      .. '"'
  end
  if type(value) == "table" then
    local parts = {}
    for key, item in pairs(value) do
      table.insert(parts, encode(tostring(key)) .. ":" .. encode(item))
    end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  error("unsupported JSON type")
end

function done(summary, latency, requests)
  local result = {
    summary = summary,
    latency_ms = {},
    invalid = 0,
    responses = 0,
    steady_responses = 0,
    steady_invalid = 0,
    bodies = {},
    lsn_refreshes = 0
  }
  for _, thread in ipairs(threads) do
    result.invalid = result.invalid + thread:get("invalid")
    result.responses = result.responses + thread:get("responses")
    result.steady_responses = result.steady_responses + thread:get("steady_responses")
    result.steady_invalid = result.steady_invalid + thread:get("steady_invalid")
    result.lsn_refreshes = result.lsn_refreshes + thread:get("refreshes")
    -- Read scalars: this upstream wrk2 revision reverses keys/values when
    -- copying Lua tables between states in script_copy_value().
    for _, key in ipairs({"127.0.0.1", "127.0.0.2", "127.0.0.3", "hosts_json"}) do
      result.bodies[key] = (result.bodies[key] or 0) + (thread:get("count_" .. key) or 0)
    end
  end
  for _, p in ipairs({50, 90, 95, 99, 99.9}) do
    result.latency_ms[tostring(p)] = latency:percentile(p) / 1000
  end
  result.latency_ms.max = latency.max / 1000
  result.latency_ms.mean = latency.mean / 1000
  io.write("BENCH_JSON " .. encode(result) .. "\n")
end
