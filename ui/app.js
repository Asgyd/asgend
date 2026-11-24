'use strict';
const $ = (id) => document.getElementById(id);
let lastTasks = [];
let token = sessionStorage.getItem('ao_ui_token') || '';
let tokenPrompt = null;
let refreshTimer = null;
let currentTaskId = null;
let chatLastEventId = 0;
let activeQuestionEventId = -1;
let chatHasAssistantMessage = false;
let activeLLMStreamId = null;
let activeLLMStreamBubble = null;
let activeLLMStreamContent = '';
const activeThinkingProcesses = new Map();
const activeToolCallProcesses = new Map();
const activeOrchestratorProcesses = new Map();
let workspaceCurrentPath = '';
let workspaceSelectedFile = '';
let workspaceOriginalContent = '';
let workspaceDirty = false;
let workspaceSaving = false;
let workspaceEntries = [];
function setTheme(theme){const selected=theme==='light'?'light':'dark';document.body.dataset.theme=selected;localStorage.setItem('ao_ui_theme',selected);const button=$('themeButton');if(button)button.textContent=selected==='light'?'Тёмная тема':'Светлая тема'}
setTheme(localStorage.getItem('ao_ui_theme')||'dark');

function toast(message){const e=$('toast');e.textContent=message;e.classList.add('show');setTimeout(()=>e.classList.remove('show'),3000)}
async function ensureToken(){
  if(token)return token;
  if(!tokenPrompt){tokenPrompt=Promise.resolve(prompt('Введите токен доступа из ui.auth_token в config.json')).then(value=>{token=value||'';if(token)sessionStorage.setItem('ao_ui_token',token);return token}).finally(()=>{tokenPrompt=null})}
  return tokenPrompt;
}
async function api(path,options={}){
  await ensureToken();
  if(!token)throw new Error('Токен доступа не задан');
  const headers={...(options.headers||{}),'X-AO-Token':token};
  if(options.body)headers['Content-Type']='application/json';
  const response=await fetch(path,{...options,headers,cache:'no-store'});
  const data=await response.json().catch(()=>({ok:false,error:'Некорректный ответ сервера'}));
  if(response.status===401){sessionStorage.removeItem('ao_ui_token');token='';throw new Error('Неверный токен доступа. Введите его повторно.')}
  if(!response.ok||data.ok===false)throw new Error(data.error||`HTTP ${response.status}`);
  return data;
}
function setConnection(ok,text){$('connection').textContent=text;$('connection').className=`pill ${ok?'ok':'bad'}`}
function statusLabel(s){return ({queued:'В очереди',running:'Выполняется',waiting_input:'Ждёт вашего ответа',completed:'Завершена',completed_without_model:'Завершена без LLM',failed:'Ошибка',cancelled:'Отменена'})[s]||s}
function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function fmtDate(s){if(!s)return '—';const d=new Date(s);return Number.isNaN(d.valueOf())?s:d.toLocaleString()}
function shortTime(s){if(!s)return '';const d=new Date(s);return Number.isNaN(d.valueOf())?s:d.toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'})}
function activateView(name, loadChat=true){
  if(name==='chat'&&!currentTaskId){$('chatEmpty').classList.remove('hidden');$('taskChatEvents').classList.add('hidden')}
  document.querySelectorAll('.view').forEach(v=>v.classList.toggle('active',v.id===`view-${name}`));
  document.querySelectorAll('[data-view-target]').forEach(b=>b.classList.toggle('active',b.dataset.viewTarget===name));
  sessionStorage.setItem('ao_active_view_v2',name);
  if(name==='chat'&&currentTaskId&&loadChat)loadTaskChat(false);
}
document.querySelectorAll('[data-view-target]').forEach(b=>b.addEventListener('click',()=>activateView(b.dataset.viewTarget)));
document.querySelectorAll('[data-settings-target]').forEach(b=>b.addEventListener('click',()=>{const target=b.dataset.settingsTarget;document.querySelectorAll('.settings-view').forEach(v=>v.classList.toggle('active',v.id===`settings-${target}`));document.querySelectorAll('[data-settings-target]').forEach(x=>x.classList.toggle('active',x===b));sessionStorage.setItem('ao_settings_section',target)}));
async function loadAgents(){return api('/api/agents')}
async function loadHealth(){const data=await api('/api/health');const counts=data.tasks_by_status||{};$('m-queued').textContent=counts.queued||0;$('m-running').textContent=counts.running||0;$('m-completed').textContent=(counts.completed||0)+(counts.completed_without_model||0);$('m-failed').textContent=counts.failed||0;$('m-workers').textContent=data.workers??'—'}
function renderWorkspaceTaskList(tasks){
  const host=$('workspaceTaskList');if(!host)return;
  if(!tasks.length){host.innerHTML='<p class="empty">Задач пока нет</p>';return}
  host.innerHTML=tasks.map(t=>{
    const title=t.title||t.task_id||'Задача';
    const active=t.task_id===currentTaskId?' active':'';
    return `<button type="button" class="workspace-task-item${active}" data-workspace-task="${esc(t.task_id)}"><span class="workspace-task-title">${esc(title)}</span><span class="workspace-task-meta"><span>${esc(t.task_id)}</span><span class="status ${esc(t.status)}">${esc(statusLabel(t.status))}</span></span></button>`;
  }).join('');
}
function renderTasks(tasks){
  lastTasks=tasks;renderWorkspaceTaskList(tasks);const body=$('tasksBody');
  if(!tasks.length){body.innerHTML='<tr><td colspan="6" class="empty">Задач пока нет</td></tr>';return}
  body.innerHTML=tasks.map(t=>`<tr class="task-row"><td><button class="task-id" data-detail="${esc(t.task_id)}">${esc(t.title||t.task_id)}</button><span class="task-session">${esc(t.task_id)}${t.session_id?` · сессия: ${esc(t.session_id)}`:''}</span></td><td><span class="status ${esc(t.status)}">${esc(statusLabel(t.status))}</span></td><td><strong>${Number(t.priority??0)}</strong>${Number(t.effective_priority??t.priority??0)!==Number(t.priority??0)?` <small>→ ${Number(t.effective_priority)}</small>`:''}</td><td>${t.attempts}/${t.max_attempts}</td><td>${esc(fmtDate(t.created_at))}</td><td><div class="row-actions"><button class="secondary small-button" data-detail="${esc(t.task_id)}">Открыть чат</button>${['queued','running','waiting_input'].includes(t.status)?`<button class="secondary small-button" data-cancel="${esc(t.task_id)}">Отменить</button>`:''}</div></td></tr>`).join('');
}
async function loadTasks(){const status=$('statusFilter').value;const data=await api('/api/tasks'+(status?'?status='+encodeURIComponent(status):''));renderTasks(data.tasks||[])}
let currentPlanItems=[];
function activePlanScope(){return currentTaskId||'global'}
function renderPlanItems(kind){
  const host=$(kind==='todo'?'todoItems':'goalItems');
  const items=currentPlanItems.filter(x=>x.kind===kind);
  if(!items.length){host.innerHTML='<p class="plan-empty">Пока нет пунктов</p>';return}
  host.innerHTML=items.map(item=>`<div class="plan-item${item.status==='done'?' done':''}" data-plan-item="${Number(item.item_id)}"><input type="checkbox" data-plan-toggle="${Number(item.item_id)}" ${item.status==='done'?'checked':''} aria-label="Отметить пункт завершённым"><span class="plan-item-title">${esc(item.title)}</span><button type="button" class="plan-item-delete" data-plan-delete="${Number(item.item_id)}" aria-label="Удалить пункт" title="Удалить">×</button></div>`).join('');
}
async function loadPlan(){
  const scope=activePlanScope();$('planScopeLabel').textContent=scope==='global'?'Общий план':'План задачи: '+scope;
  const data=await api('/api/plan?scope_id='+encodeURIComponent(scope));currentPlanItems=Array.isArray(data.items)?data.items:[];renderPlanItems('todo');renderPlanItems('goal');
}
async function mutatePlan(action,body){
  await api('/api/plan',{method:'POST',body:JSON.stringify({action,scope_id:activePlanScope(),...body})});
  await loadPlan();
}
$('todoForm').addEventListener('submit',async e=>{e.preventDefault();const title=$('todoTitle').value.trim();if(!title)return;try{await mutatePlan('add',{kind:'todo',title});$('todoTitle').value=''}catch(err){toast(err.message)}});
$('goalForm').addEventListener('submit',async e=>{e.preventDefault();const title=$('goalTitle').value.trim();if(!title)return;try{await mutatePlan('add',{kind:'goal',title});$('goalTitle').value=''}catch(err){toast(err.message)}});
$('todoItems').addEventListener('change',async e=>{const box=e.target.closest('[data-plan-toggle]');if(!box)return;try{const item=currentPlanItems.find(x=>Number(x.item_id)===Number(box.dataset.planToggle));if(item)await mutatePlan('update',{item_id:Number(item.item_id),status:box.checked?'done':'open'})}catch(err){toast(err.message)}});
$('goalItems').addEventListener('change',async e=>{const box=e.target.closest('[data-plan-toggle]');if(!box)return;try{const item=currentPlanItems.find(x=>Number(x.item_id)===Number(box.dataset.planToggle));if(item)await mutatePlan('update',{item_id:Number(item.item_id),status:box.checked?'done':'open'})}catch(err){toast(err.message)}});
for(const hostId of ['todoItems','goalItems'])$(hostId).addEventListener('click',async e=>{const button=e.target.closest('[data-plan-delete]');if(!button)return;try{await mutatePlan('delete',{item_id:Number(button.dataset.planDelete)})}catch(err){toast(err.message)}});
$('refreshPlan').addEventListener('click',()=>loadPlan().catch(e=>toast(e.message)));
async function refresh(){try{await Promise.all([loadHealth(),loadTasks()]);setConnection(true,'Подключено');if(currentTaskId&&$('view-chat').classList.contains('active'))await loadTaskChat(false)}catch(e){setConnection(false,'Нет соединения');if(e.message.includes('Токен'))toast(e.message);throw e}}

const EVENT_TITLES={
  task_created:'Вы',user_answer:'Вы',agent_message:'Ответ',agent_question:'Нужно уточнение',question:'Нужно уточнение',
  orchestration_started:'Оркестратор',agent_selected:'Профиль исполнения выбран',analysis_started:'Анализ задачи',
  orchestrator_waiting_input:'Ожидание вашего ответа',decomposition_completed:'План работ проверен',subtask_started:'Этап работы',
  resources_selected:'Ресурсы подобраны',agent_started:'Выполнение началось',agent_completed:'Этап завершён',task_resumed:'Продолжаю после ответа',
  model_selected:'Выбрана модель',agent_llm_request:'Вызов модели',tool_started:'Вызов инструмента',tool_completed:'Инструмент завершён',
  model_released:'Модель выгружена',model_release_warning:'Не удалось выгрузить модель',task_completed:'Задача завершена',task_failed:'Ошибка выполнения',
  task_cancel_requested:'Запрошена отмена',task_cancelled:'Задача отменена',task_title_updated:'Название задачи сохранено',task_workspace_created:'Рабочая папка создана',task_workspace_active:'Рабочая папка подключена',task_tier_selected:'Выбран уровень модели'
};
const KEY_LABELS={content:'Сообщение',summary:'Итог',answer:'Ответ',question:'Вопрос',status:'Статус',reason:'Причина',assumption:'Предположение',assumptions:'Допущения',
  known_information:'Известная информация',constraints:'Ограничения',issues:'Замечания',defects:'Недочёты',unverified_claims:'Не удалось проверить',changes:'Изменения',
  score:'Оценка качества',output:'Результат',error:'Ошибка',tool:'Инструмент',skills:'Навыки',system_prompts:'Системные инструкции',tools:'Инструменты',knowledge:'База знаний',
  model:'Модель',turn:'Попытка',title:'Этап',acceptance_criteria:'Критерии готовности',instruction:'Инструкция'};
function humanLabel(key){return KEY_LABELS[key]||String(key).replace(/_/g,' ').replace(/^./,x=>x.toUpperCase())}
function humanizeValue(value,depth=0){
  if(value===null||value===undefined)return '';
  if(typeof value==='string'){
    const text=value.trim();
    if(depth<3&&(text.startsWith('{')||text.startsWith('['))){try{return humanizeValue(JSON.parse(text),depth+1)}catch{}}
    return value;
  }
  if(typeof value==='number'||typeof value==='boolean')return typeof value==='boolean'?(value?'Да':'Нет'):String(value);
  if(Array.isArray(value))return value.map(v=>{const h=humanizeValue(v,depth+1);return h?`• ${h}`:''}).filter(Boolean).join('\n');
  if(typeof value==='object'){
    const entries=Object.entries(value).filter(([k,v])=>!['id','object','index','finish_reason'].includes(k)&&v!==null&&v!==undefined&&v!=='');
    return entries.map(([k,v])=>{
      const text=humanizeValue(v,depth+1);if(!text)return '';
      if(k==='content'&&entries.length===1)return text;
      if(['verification','review'].includes(k))return `${humanLabel(k)}\n${text}`;
      return `${humanLabel(k)}: ${text}`;
    }).filter(Boolean).join('\n');
  }
  return String(value);
}
function readableText(raw){
  if(raw===null||raw===undefined)return '';
  if(typeof raw!=='string')return humanizeValue(raw);
  const text=raw.trim();
  if(text.startsWith('{')||text.startsWith('[')){
    try{const parsed=JSON.parse(text);if(parsed&&typeof parsed==='object'&&(parsed.type==='question'||parsed.need_user_input===true))return '';return humanizeValue(parsed)}catch{}
  }
  return raw;
}
function inlineMarkdown(text){return text.replace(/`([^`]+)`/g,'<code>$1</code>').replace(/\*\*([^*]+)\*\*/g,'<strong>$1</strong>').replace(/(^|[^*])\*([^*\n]+)\*/g,'$1<em>$2</em>')}
function markdownHtml(raw){
  const lines=esc(String(raw??'')).split('\n');let html='',paragraph=[],list=[];
  const flushP=()=>{if(paragraph.length){html+=`<p>${inlineMarkdown(paragraph.join('<br>'))}</p>`;paragraph=[]}};
  const flushL=()=>{if(list.length){html+=`<ul>${list.map(x=>`<li>${inlineMarkdown(x)}</li>`).join('')}</ul>`;list=[]}};
  for(const line of lines){const bullet=line.match(/^\s*[-*+]\s+(.+)$/);const heading=line.match(/^(#{1,3})\s+(.+)$/);
    if(bullet){flushP();list.push(bullet[1]);continue}flushL();
    if(heading){flushP();const level=heading[1].length;html+=`<h${level+2}>${inlineMarkdown(heading[2])}</h${level+2}>`;continue}
    if(!line.trim()){flushP();continue}paragraph.push(line);
  }
  flushP();flushL();return html||'<p class="muted-text">Нет текста.</p>';
}
function appendSystemEvent(title,message,time=''){
  const el=document.createElement('details');el.className='chat-process';
  const summary=document.createElement('summary');
  const icon=document.createElement('span');icon.className='chat-activity-mark';icon.textContent='•';
  const head=document.createElement('strong');head.textContent=title;
  const clock=document.createElement('time');clock.textContent=shortTime(time);
  summary.append(icon,head,clock);el.appendChild(summary);
  if(message){const body=document.createElement('div');body.className='chat-process-body';body.textContent=message;el.appendChild(body)}
  $('taskChatEvents').appendChild(el);return el;
}
function appendLiveProcess(map,key,title,time=''){
  let state=map.get(key);if(state)return state;
  const el=document.createElement('details');el.className='chat-process chat-live-process';el.open=true;
  const summary=document.createElement('summary'),icon=document.createElement('span');icon.className='chat-activity-mark';icon.textContent='◌';
  const head=document.createElement('strong');head.textContent=title;
  const clock=document.createElement('time');clock.textContent=shortTime(time);summary.append(icon,head,clock);
  const body=document.createElement('div');body.className='chat-process-body chat-live-process-body';
  el.append(summary,body);$('taskChatEvents').appendChild(el);
  state={el,head,body,text:'',title};map.set(key,state);return state;
}
function appendThinkingDelta(event){
  const payload=event.payload||{},key=String(payload.stream_id||'unknown');
  const state=appendLiveProcess(activeThinkingProcesses,key,'Модель формирует thinking/reasoning',event.created_at||'');
  const delta=typeof payload.delta==='string'?payload.delta:'';
  const deltaBytes=Number(payload.delta_bytes)||new TextEncoder().encode(delta).length;
  if(deltaBytes>0){
    state.byteCount=(state.byteCount||0)+deltaBytes;
    state.head.textContent=`Thinking/reasoning · генерация · ${state.byteCount} байт`;
    /* Display progress only, never the model's private reasoning text. */
    state.body.textContent='Модель генерирует внутренние reasoning-токены. Их содержимое скрыто; сегмент можно свернуть или развернуть.';
  }
  $('taskChatEvents').scrollTop=$('taskChatEvents').scrollHeight;
}
function appendToolCallDelta(event){
  const payload=event.payload||{},key=String(payload.call_key||`${payload.stream_id||'stream'}-${payload.index||0}`);
  const name=String(payload.tool_name||'');
  const state=appendLiveProcess(activeToolCallProcesses,key,`Модель формирует вызов инструмента${name?`: ${name}`:''}`,event.created_at||'');
  if(name)state.head.textContent=`Модель формирует вызов инструмента: ${name}`;
  state.body.textContent='Аргументы вызова формируются…';
  state.toolName=name;state.el.dataset.toolName=name;
  $('taskChatEvents').scrollTop=$('taskChatEvents').scrollHeight;
}
function finishStreamProcesses(streamId){
  for(const [key,state] of activeThinkingProcesses){if(key===streamId){state.el.open=false;state.head.textContent=`Thinking/reasoning · завершено (${state.byteCount||0} байт)`;activeThinkingProcesses.delete(key)}}
  for(const [key,state] of activeToolCallProcesses){if(key.startsWith(`${streamId}-`)){state.el.open=false;state.head.textContent=`Вызов инструмента${state.toolName?`: ${state.toolName}`:''} · сформирован`;state.body.textContent='Ожидается выполнение оркестратором.'}}
  const orchestrator=activeOrchestratorProcesses.get(streamId);if(orchestrator){orchestrator.el.open=false;orchestrator.head.textContent='Внутренний шаг оркестратора · завершён';activeOrchestratorProcesses.delete(streamId)}
}
function findToolProcessByName(name){for(const [key,state] of activeToolCallProcesses){if(state.toolName===name)return [key,state]}return null}
function appendBubble(kind,title,time,content){
  if(!content)return;
  const el=document.createElement('article');el.className=`chat-bubble ${kind}`;
  const head=document.createElement('div');head.className='chat-bubble-head';
  const name=document.createElement('strong');name.textContent=title;
  const stamp=document.createElement('time');stamp.textContent=shortTime(time);
  head.append(name,stamp);
  const body=document.createElement('div');body.className='chat-bubble-body';body.innerHTML=markdownHtml(content);
  el.append(head,body);$('taskChatEvents').appendChild(el);
  if(kind==='assistant')chatHasAssistantMessage=true;
  return el;
}
function questionsFromPayload(payload){if(!payload||typeof payload!=='object')return [];if(Array.isArray(payload.questions))return payload.questions.filter(q=>q&&typeof q.question==='string');const q=payload.question&&typeof payload.question==='object'?payload.question:payload;return typeof q.question==='string'?[q]:[]}
function renderChatEvent(event){
  const type=String(event.type||'event'),payload=event.payload&&typeof event.payload==='object'?event.payload:{};
  const actor=String(event.actor||'orchestrator'),time=event.created_at||'';
  if(type==='llm_status'){
    const status=String(payload.status||'unknown'),streamId=String(payload.stream_id||'');
    const streamKind=String(payload.stream_kind||'assistant');
    if(status==='connecting'){
      activeLLMStreamId=streamId;activeLLMStreamBubble=null;activeLLMStreamContent='';
      if(streamKind==='orchestrator')appendLiveProcess(activeOrchestratorProcesses,streamId,'Оркестратор выполняет внутренний этап',time);
      else appendSystemEvent('Подключение к модели','Подключение к выбранной модели.',time);return;
    }
    if(status==='waiting-first-token'){
      if(streamKind==='orchestrator')appendLiveProcess(activeOrchestratorProcesses,streamId,'Оркестратор ожидает внутренний ответ модели',time);
      else appendSystemEvent('Ожидание ответа модели','Подключение установлено; ожидается первый токен.',time);return;
    }
    if(status==='stalled'){
      const state=activeThinkingProcesses.get(streamId);if(state){state.head.textContent='Thinking · поток остановился';state.el.open=false;}
      appendSystemEvent('Генерация приостановилась',event.message||'Поток модели перестал выдавать данные.',time);return;
    }
    if(status==='HTTP-error'){
      finishStreamProcesses(streamId);
      appendSystemEvent('Ошибка модели',event.message||'Запрос к модели завершился ошибкой.',time);activeLLMStreamBubble=null;return;
    }
    if(status==='completed'){
      finishStreamProcesses(streamId);
      if(streamKind==='orchestrator'){
        const state=activeOrchestratorProcesses.get(streamId);if(state){state.el.open=false;state.head.textContent='Внутренний шаг оркестратора · завершён';activeOrchestratorProcesses.delete(streamId)}
        activeLLMStreamBubble=null;activeLLMStreamId=null;activeLLMStreamContent='';return;
      }
      const tokens=Number(payload.completion_tokens);const suffix=Number.isFinite(tokens)&&tokens>=0?`Завершено · ${tokens} токенов`:'Генерация завершена.';
      appendSystemEvent('Ответ модели получен',suffix,time);activeLLMStreamBubble=null;activeLLMStreamId=null;activeLLMStreamContent='';return;
    }
    return;
  }
  if(type==='assistant_delta'){
    const delta=typeof payload.delta==='string'?payload.delta:'';if(!delta)return;
    const streamId=String(payload.stream_id||'');
    if(!activeLLMStreamId||activeLLMStreamId!==streamId){activeLLMStreamId=streamId;activeLLMStreamBubble=null;activeLLMStreamContent=''}
    activeLLMStreamContent+=delta;
    if(!activeLLMStreamBubble)activeLLMStreamBubble=appendBubble('assistant','ASGEND',time,activeLLMStreamContent);
    else {const body=activeLLMStreamBubble.querySelector('.chat-bubble-body');if(body)body.innerHTML=markdownHtml(activeLLMStreamContent)}
    $('taskChatEvents').scrollTop=$('taskChatEvents').scrollHeight;
    return;
  }
  if(type==='orchestrator_model_delta'){
    const streamId=String(payload.stream_id||'unknown'),state=appendLiveProcess(activeOrchestratorProcesses,streamId,'Оркестратор формирует внутренний результат',time);
    const delta=typeof payload.delta==='string'?payload.delta:'';if(delta){state.text+=delta;if(state.text.length>120000)state.text='…\n'+state.text.slice(-120000);state.body.textContent=state.text;}
    $('taskChatEvents').scrollTop=$('taskChatEvents').scrollHeight;return;
  }
  if(type==='llm_thinking_delta'){appendThinkingDelta(event);return;}
  if(type==='tool_call_stream_delta'){appendToolCallDelta(event);return;}
  if(type==='orchestrator_waiting_input'){
    const questions=questionsFromPayload(payload);
    appendSystemEvent('Оркестратор ожидает уточнения',`${questions.length||1} вопрос(ов) перед продолжением задачи`,time);
    if(questions.length)showQuestion(event.event_id,questions);return;
  }
  if(type==='task_title_updated'){const title=String(payload.title||'');if(title){$('chatTaskTitle').textContent=title;const item=lastTasks.find(t=>t.task_id===currentTaskId);if(item)item.title=title;renderWorkspaceTaskList(lastTasks)}appendSystemEvent('Название задачи сохранено',title||event.message,time);return}
  if(type==='task_workspace_created'||type==='task_workspace_active'){const path=String(payload.workspace_path||'');if(path)$('workspacePath').textContent=`Рабочая папка: ${path}`;appendSystemEvent(type==='task_workspace_created'?'Рабочая папка создана':'Рабочая папка подключена',path||event.message,time);return}
  if(type==='task_created'){appendBubble('user','Вы',time,event.message||'Новая задача');return}
  if(type==='user_answer'){appendBubble('user','Вы',time,readableText(payload.answer||event.message));return}
  if(type==='agent_message'){
    const content=readableText(typeof payload.content==='string'?payload.content:event.message);
    if(content)appendBubble('assistant',actor==='orchestrator'?'Оркестратор':'ASGEND',time,content);
    return;
  }
  if(type==='agent_question'||type==='question'){
    const questions=questionsFromPayload(payload);
    const summary=questions.map((q,i)=>`${i+1}. ${q.question}`).join('\n');
    appendBubble('question','Уточнение',time,summary||(event.message||'Нужно уточнение'));
    if(questions.length)showQuestion(event.event_id,questions);return;
  }
  if(type==='tool_started'){
    const tool=String(payload.tool||((event.message||'').match(/tool ['"]([^'"]+)/)||[])[1]||'');
    const found=findToolProcessByName(tool);
    if(found){const [key,state]=found;state.el.open=false;state.head.textContent=`Инструмент: ${tool} · выполняется`;state.body.textContent='Оркестратор выполняет вызов инструмента.';activeToolCallProcesses.delete(key);activeToolCallProcesses.set(`running-${tool}`,state);}
    else appendSystemEvent('Инструмент запускается',tool||event.message,time);return;
  }
  if(type==='tool_completed'){
    const tool=String(payload.tool||'инструмент');const state=activeToolCallProcesses.get(`running-${tool}`);
    const output=payload.error?`Ошибка: ${payload.error}`:payload.output?readableText(payload.output):'Вызов завершён.';
    if(state){state.head.textContent=`Инструмент: ${tool} · завершён`;state.body.textContent=output;state.el.open=false;activeToolCallProcesses.delete(`running-${tool}`)}
    else appendSystemEvent(`Инструмент: ${tool} · завершён`,output||event.message,time);return;
  }
  if(type==='resources_selected'){
    const count=(key)=>Array.isArray(payload[key])?payload[key].length:0;
    const brief=[['skills','навыков'],['system_prompts','инструкций'],['tools','инструментов'],['knowledge','источников знаний']].filter(([k])=>count(k)).map(([k,n])=>`${count(k)} ${n}`).join(' · ');
    appendSystemEvent('Ресурсы подобраны',brief||'Подбор ресурсов завершён',time);return;
  }
  if(type==='decomposition_completed'){
    const score=Number(payload.score);appendSystemEvent('План проверен',Number.isFinite(score)&&score>0?`Оценка качества: ${score}/10`:event.message,time);return;
  }
  if(type==='command_safety_check'){
    const allowed=payload.allowed===true,title=allowed?'Проверка безопасности команды · разрешено':'Проверка безопасности команды · заблокировано';
    appendSystemEvent(title,`${payload.tool||'Инструмент'} · риск: ${payload.risk||'не указан'} · ${payload.reason||event.message||''}`,time);return;
  }
  if(type==='agent_loop_check'||type==='agent_loop_detected'){
    appendSystemEvent(type==='agent_loop_detected'?'Обнаружен цикл агента':'Проверка повторяющегося действия',event.message||JSON.stringify(payload),time);return;
  }
  if(type==='timeout_diagnostic'||type==='problem_classification'){
    const title=type==='timeout_diagnostic'?'Диагностика тайм-аута':'Классификация проблемы';
    const text=[payload.category,payload.likely_cause,payload.recommended_action,payload.elapsed_ms!==undefined?`Время: ${payload.elapsed_ms} мс`:null].filter(Boolean).join('\n');
    appendSystemEvent(title,text||event.message,time);return;
  }
  if(type==='agent_llm_request'){
    appendSystemEvent('Модель вызвана',event.message||'Ожидается ответ модели',time);return;
  }
  if(['agent_started','agent_completed','model_selected','model_released','model_release_warning','task_resumed','task_cancel_requested','task_cancelled','orchestration_started','analysis_started','orchestrator_waiting_input','task_completed','task_failed','subtask_started','agent_selected'].includes(type)){
    appendSystemEvent(EVENT_TITLES[type]||type,event.message||'',time);return;
  }
  appendSystemEvent(EVENT_TITLES[type]||'Событие',event.message||'',time);
}
function showQuestion(eventId,questionInput){
  const questions=Array.isArray(questionInput)?questionInput:(questionInput&&Array.isArray(questionInput.questions)?questionInput.questions:[questionInput]);
  const valid=questions.filter(q=>q&&typeof q.question==='string');
  const id=Number(eventId);if(!currentTaskId||!valid.length||(id>=0&&activeQuestionEventId===id)||$('questionDialog').open)return;
  activeQuestionEventId=Number.isFinite(id)&&id>=0?id:-1;
  $('questionTitle').textContent=valid.length>1?`Нужно уточнить ${valid.length} вопроса`:'Уточнение по задаче';
  $('questionContext').textContent=`Задача ${currentTaskId}. Ответьте на все вопросы; оркестратор продолжит выполнение после отправки ответов.`;
  const host=$('questionList');host.innerHTML='';
  valid.forEach((question,index)=>{
    const fieldset=document.createElement('fieldset');fieldset.className='clarification-question';fieldset.dataset.questionIndex=String(index);
    const legend=document.createElement('legend');legend.textContent=`${index+1}. ${question.question}`;fieldset.appendChild(legend);
    const options=document.createElement('div');options.className='question-options';
    for(const option of (question.options||[])){
      const label=typeof option==='string'?option:(option.label||option.id||'Вариант');
      const button=document.createElement('button');button.type='button';button.className='question-option';button.textContent=label;button.dataset.optionId=typeof option==='string'?'':(option.id||'');
      button.addEventListener('click',()=>{options.querySelectorAll('.question-option').forEach(b=>b.classList.toggle('selected',b===button));answer.value=label;answer.dataset.optionId=button.dataset.optionId});
      options.appendChild(button);
    }
    if(options.children.length)fieldset.appendChild(options);
    const answer=document.createElement('textarea');answer.className='clarification-answer';answer.dataset.questionIndex=String(index);answer.rows=2;answer.maxLength=10000;answer.placeholder='Ответ на этот вопрос…';answer.setAttribute('aria-label',`Ответ на вопрос ${index+1}`);fieldset.appendChild(answer);
    host.appendChild(fieldset);
  });
  if(typeof $('questionDialog').showModal==='function')$('questionDialog').showModal();
}

function workspaceParentPath(path){const parts=String(path||'').split('/').filter(Boolean);parts.pop();return parts.join('/')}
function updateWorkspaceEditorState(){
  const state=$('workspaceEditorState');if(!state)return;
  state.textContent=workspaceDirty?'Не сохранено':workspaceSelectedFile?'Сохранено':'Не выбран';
  state.className=`pill ${workspaceDirty?'warn':workspaceSelectedFile?'ok':'muted'}`;
  $('workspaceSaveFile').disabled=!currentTaskId||!workspaceSelectedFile||!workspaceDirty;
}
function clearWorkspaceEditor(){
  workspaceSelectedFile='';workspaceOriginalContent='';workspaceDirty=false;
  if($('workspaceFileEditor'))$('workspaceFileEditor').value='';
  if($('workspaceEditorName'))$('workspaceEditorName').textContent='Файл не выбран';
  if($('workspaceEditorPanel'))$('workspaceEditorPanel').classList.add('hidden');
  if($('workspaceFileMessage'))$('workspaceFileMessage').textContent='';
  updateWorkspaceEditorState();
}
function confirmWorkspaceDiscard(action='продолжить'){
  if(workspaceSaving){toast('Дождитесь завершения сохранения файла');return false}
  if(!workspaceDirty)return true;
  return confirm(`Есть несохранённые изменения. Отбросить их и ${action}?`);
}
async function workspaceRequest(action,fields={},taskId=currentTaskId){
  if(!taskId)throw new Error('Сначала выберите задачу');
  return api(`/api/tasks/${encodeURIComponent(taskId)}/files`,{method:'POST',body:JSON.stringify({action,...fields})});
}
function renderWorkspaceFiles(data){
  const host=$('workspaceFiles');workspaceEntries=data.entries||[];
  $('workspaceFilePath').textContent=data.path?`/${data.path}`:'/';
  $('workspaceFolderUp').disabled=!data.path;
  const entries=workspaceEntries.slice().sort((a,b)=>(a.kind==='directory'?0:1)-(b.kind==='directory'?0:1)||a.name.localeCompare(b.name));
  if(!entries.length){host.innerHTML='<p class="empty">В этой папке пока нет файлов.</p>';return}
  host.innerHTML=entries.map(item=>{
    const path=[data.path,item.name].filter(Boolean).join('/');
    const active=workspaceSelectedFile===path?' active':'';
    const icon=item.kind==='directory'?'▸':'▤';
    const size=item.kind==='file'?formatFileSize(item.size):'';
    return `<button type="button" class="workspace-file-item${active}" data-workspace-${item.kind==='directory'?'dir':'file'}="${esc(path)}"><span class="workspace-file-icon">${icon}</span><span class="workspace-file-name" title="${esc(item.name)}">${esc(item.name)}</span><span class="workspace-file-size">${esc(size)}</span></button>`;
  }).join('');
  if(data.truncated)host.insertAdjacentHTML('beforeend','<p class="footnote">Показаны первые 500 записей.</p>');
}
function formatFileSize(value){const n=Number(value)||0;if(n<1024)return `${n} Б`;if(n<1024*1024)return `${(n/1024).toFixed(1)} КиБ`;return `${(n/1024/1024).toFixed(1)} МиБ`}
async function loadWorkspaceFiles(path=workspaceCurrentPath){
  if(!currentTaskId){$('workspaceFiles').innerHTML='<p class="empty">Выберите задачу для просмотра файлов.</p>';return}
  const requestedPath=String(path||''),taskId=currentTaskId;
  const discardOnSuccess=workspaceDirty&&requestedPath!==workspaceCurrentPath;
  if(discardOnSuccess&&!confirmWorkspaceDiscard('перейти в другую папку'))return;
  if(workspaceSaving&&requestedPath!==workspaceCurrentPath){toast('Дождитесь завершения сохранения файла');return}
  try{
    const data=await workspaceRequest('list',{path:requestedPath},taskId);
    if(taskId!==currentTaskId)return;
    if(discardOnSuccess)clearWorkspaceEditor();
    workspaceCurrentPath=data.path||'';renderWorkspaceFiles(data);$('workspaceFileMessage').textContent='';
  }catch(error){if(taskId!==currentTaskId)return;$('workspaceFiles').innerHTML=`<p class="empty">${esc(error.message)}</p>`;workspaceEntries=[];$('workspaceFolderUp').disabled=!workspaceCurrentPath}
}
async function openWorkspaceFile(path){
  if(!confirmWorkspaceDiscard('открыть другой файл'))return;
  const taskId=currentTaskId;
  try{
    const data=await workspaceRequest('read',{path},taskId);if(taskId!==currentTaskId)return;
    workspaceSelectedFile=path;workspaceOriginalContent=data.content||'';workspaceDirty=false;$('workspaceEditorPanel').classList.remove('hidden');$('workspaceEditorName').textContent=path.split('/').pop();$('workspaceFileEditor').value=workspaceOriginalContent;$('workspaceFileMessage').textContent=`${formatFileSize(data.size)} · UTF-8`;updateWorkspaceEditorState();renderWorkspaceFiles({path:workspaceCurrentPath,entries:workspaceEntries});
  }catch(error){toast(error.message)}
}
async function saveWorkspaceFile(){
  if(!workspaceSelectedFile||!currentTaskId||workspaceSaving)return;
  const taskId=currentTaskId,path=workspaceSelectedFile,content=$('workspaceFileEditor').value,button=$('workspaceSaveFile');
  workspaceSaving=true;button.disabled=true;$('workspaceFileMessage').textContent='Сохраняем…';
  try{
    const data=await workspaceRequest('write',{path,content},taskId);
    if(taskId!==currentTaskId||path!==workspaceSelectedFile)return;
    workspaceOriginalContent=content;workspaceDirty=$('workspaceFileEditor').value!==content;$('workspaceFileMessage').textContent=`Сохранено · ${formatFileSize(data.size)}`;updateWorkspaceEditorState();
    if(!workspaceDirty)await loadWorkspaceFiles(workspaceCurrentPath);toast('Файл сохранён');
  }catch(error){if(taskId===currentTaskId&&path===workspaceSelectedFile){$('workspaceFileMessage').textContent=error.message;toast(error.message);updateWorkspaceEditorState()}}
  finally{workspaceSaving=false;if(taskId===currentTaskId&&path===workspaceSelectedFile)updateWorkspaceEditorState()}
}
async function createWorkspaceFile(){
  const name=$('workspaceNewFileName').value.trim();
  if(!currentTaskId){toast('Сначала выберите задачу');return}
  if(!name||name==='.'||name==='..'||name.includes('/')||name.includes('\\')||/[\x00-\x1f]/.test(name)){toast('Введите безопасное имя файла без пути');return}
  if(workspaceEntries.some(x=>x.name===name)){toast('Файл или папка с таким именем уже существует');return}
  if(!confirmWorkspaceDiscard('создать новый файл'))return;
  const discardDirty=workspaceDirty,path=[workspaceCurrentPath,name].filter(Boolean).join('/'),taskId=currentTaskId;
  workspaceSaving=true;
  try{
    await workspaceRequest('write',{path,content:''},taskId);if(taskId!==currentTaskId)return;
    if(discardDirty)clearWorkspaceEditor();$('workspaceNewFileName').value='';workspaceSaving=false;
    await loadWorkspaceFiles(workspaceCurrentPath);await openWorkspaceFile(path);toast('Файл создан');
  }catch(error){toast(error.message)}
  finally{workspaceSaving=false;updateWorkspaceEditorState()}
}
function appendFallbackResult(result,time=''){
  if(!result)return;
  const outputs=Array.isArray(result.agent_results)?result.agent_results:[];
  let output='';
  for(const item of outputs){if(item&&item.output!==undefined){output=readableText(item.output);if(output)break}}
  if(!output&&result.error)output=String(result.error);
  if(output)appendBubble('assistant','Результат задачи',time,output);
}
async function loadTaskChat(reset=false){
  if(!currentTaskId)return;const id=currentTaskId;
  try{
    const data=await api('/api/tasks/'+encodeURIComponent(id));if(id!==currentTaskId)return;
    $('chatTaskTitle').textContent=data.title||data.task||data.original_task||'Чат задачи';$('detailId').textContent=id;$('workspacePath').textContent=data.workspace_path?`Рабочая папка: ${data.workspace_path}`:'Рабочая папка: недоступна для ранее созданной задачи';
    $('taskChatStatus').textContent=statusLabel(data.status);$('taskChatStatus').className=`status ${esc(data.status)}`;
    $('cancelChatTask').classList.toggle('hidden',!['queued','running','waiting_input'].includes(data.status));
    $('taskChatSummary').textContent=data.status==='waiting_input'?'Задача приостановлена и ждёт ваш ответ.':data.status==='running'?'Выполнение идёт; события появляются автоматически.':data.status==='queued'?'Задача сохранена в очереди и начнёт выполняться автоматически.':`Статус задачи: ${statusLabel(data.status)}.`;
    $('chatEmpty').classList.toggle('hidden',true);$('taskChatEvents').classList.remove('hidden');
    if(reset){chatLastEventId=0;activeQuestionEventId=-1;chatHasAssistantMessage=false;activeLLMStreamId=null;activeLLMStreamBubble=null;activeLLMStreamContent='';$('taskChatEvents').innerHTML='';workspaceCurrentPath='';workspaceEntries=[];clearWorkspaceEditor();renderWorkspaceTaskList(lastTasks);await loadWorkspaceFiles('');await loadPlan();}
    const events=await api(`/api/tasks/${encodeURIComponent(id)}/events?after_id=${chatLastEventId}&limit=100`);
    for(const event of (events.events||[])){chatLastEventId=Math.max(chatLastEventId,Number(event.event_id)||0);renderChatEvent(event)}
    if(data.status==='waiting_input'&&(data.result?.questions||data.result?.question))showQuestion(chatLastEventId||-1,data.result.questions||[data.result.question]);
    if(['completed','completed_without_model','failed','cancelled'].includes(data.status)&&!chatHasAssistantMessage)appendFallbackResult(data.result,data.updated_at||data.created_at);
    if(data.error&&data.status==='failed')appendSystemEvent('Ошибка выполнения',data.error,data.updated_at);
    if(!$('taskChatEvents').children.length)appendSystemEvent('Чат создан','Ожидаем первые события оркестратора.',data.created_at);
    $('taskChatEvents').scrollTop=$('taskChatEvents').scrollHeight;
  }catch(e){toast(e.message)}
}
async function showDetails(id){
  if(workspaceSaving){toast('Дождитесь завершения сохранения файла');return}
  if(currentTaskId===id){activateView('chat',false);await loadTaskChat(false);return}
  if(!confirmWorkspaceDiscard('открыть другую задачу'))return;
  if(workspaceDirty)clearWorkspaceEditor();
  currentTaskId=id;activeQuestionEventId=-1;activateView('chat',false);await loadTaskChat(true);loadCoworkProjectFiles('').catch(()=>{})
}
async function cancelTask(id){if(!confirm(`Запросить отмену задачи ${id}?`))return;try{await api('/api/tasks/'+encodeURIComponent(id)+'/cancel',{method:'POST',body:'{}'});toast('Отмена запрошена');await refresh()}catch(e){toast(e.message)}}
$('tasksBody').addEventListener('click',e=>{const detail=e.target.closest('[data-detail]');const cancel=e.target.closest('[data-cancel]');if(detail)showDetails(detail.dataset.detail);if(cancel)cancelTask(cancel.dataset.cancel)});
$('workspaceTaskList').addEventListener('click',e=>{const button=e.target.closest('[data-workspace-task]');if(button)showDetails(button.dataset.workspaceTask)});
$('workspaceNewTask').addEventListener('click',()=>{if(!confirmWorkspaceDiscard('создать новую задачу'))return;if(workspaceDirty)clearWorkspaceEditor();activateView('dashboard');$('task').focus()});
$('workspaceRefreshTasks').addEventListener('click',()=>loadTasks().catch(e=>toast(e.message)));
$('workspaceRefreshFiles').addEventListener('click',()=>loadWorkspaceFiles(workspaceCurrentPath));
$('workspaceFolderUp').addEventListener('click',()=>loadWorkspaceFiles(workspaceParentPath(workspaceCurrentPath)));
$('workspaceFiles').addEventListener('click',e=>{const dir=e.target.closest('[data-workspace-dir]');const file=e.target.closest('[data-workspace-file]');if(dir)loadWorkspaceFiles(dir.dataset.workspaceDir);else if(file)openWorkspaceFile(file.dataset.workspaceFile)});
$('workspaceFileEditor').addEventListener('input',()=>{workspaceDirty=$('workspaceFileEditor').value!==workspaceOriginalContent;updateWorkspaceEditorState()});
$('workspaceSaveFile').addEventListener('click',saveWorkspaceFile);
$('workspaceCreateFile').addEventListener('click',createWorkspaceFile);
$('workspaceNewFileName').addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();createWorkspaceFile()}});
$('refresh').addEventListener('click',()=>refresh().catch(e=>toast(e.message)));
$('themeButton').addEventListener('click',()=>setTheme(document.body.dataset.theme==='light'?'dark':'light'));
$('statusFilter').addEventListener('change',()=>loadTasks().catch(e=>toast(e.message)));
$('closeDetail').addEventListener('click',()=>{if(!confirmWorkspaceDiscard('закрыть чат'))return;if(workspaceDirty)clearWorkspaceEditor();currentTaskId=null;activeQuestionEventId=-1;$('questionDialog').close();activateView('dashboard')});
$('cancelChatTask').addEventListener('click',()=>currentTaskId&&cancelTask(currentTaskId));
$('answerDismiss').addEventListener('click',()=>$('questionDialog').close());
$('questionDialogForm').addEventListener('submit',async e=>{e.preventDefault();if(!currentTaskId)return;const fields=[...$('questionList').querySelectorAll('.clarification-question')];const answers=fields.map((field,i)=>({question:field.querySelector('legend')?.textContent||`Вопрос ${i+1}`,answer:field.querySelector('.clarification-answer')?.value.trim()||''}));if(!answers.length||answers.some(x=>!x.answer)){toast('Ответьте на каждый вопрос');return}const answer=answers.length===1?answers[0].answer:'Ответы на уточняющие вопросы:\n'+answers.map((x,i)=>`${i+1}. ${x.question} — ${x.answer}`).join('\n');const submit=$('answerSubmit');submit.disabled=true;try{await api(`/api/tasks/${encodeURIComponent(currentTaskId)}/answer`,{method:'POST',body:JSON.stringify({answer})});$('questionDialog').close();activeQuestionEventId=-1;appendBubble('user','Вы',new Date().toISOString(),answer);toast('Ответы приняты. Задача продолжится.');await refresh()}catch(err){toast(err.message)}finally{submit.disabled=false}});
$('taskFollowupForm').addEventListener('submit',async e=>{e.preventDefault();const task=$('taskFollowupText').value.trim();if(!task||!currentTaskId)return;if(!confirmWorkspaceDiscard('создать задачу-продолжение'))return;const button=$('taskFollowupSubmit');button.disabled=true;try{const previous=await api('/api/tasks/'+encodeURIComponent(currentTaskId));const payload={task,priority:5,idempotency_key:crypto.randomUUID()};if(previous.session_id)payload.session_id=previous.session_id;const created=await api('/api/tasks',{method:'POST',body:JSON.stringify(payload)});$('taskFollowupText').value='';if(workspaceDirty)clearWorkspaceEditor();await refresh();await showDetails(created.task_id)}catch(err){toast(err.message)}finally{button.disabled=false}});
$('submitForm').addEventListener('submit',async e=>{e.preventDefault();const task=$('task').value.trim();if(!task)return;if(!confirmWorkspaceDiscard('отправить новую задачу'))return;const button=$('submitButton');button.disabled=true;$('submitMessage').textContent='Сохраняем…';try{const payload={task,priority:Number($('priority').value),idempotency_key:crypto.randomUUID()};if($('session').value.trim())payload.session_id=$('session').value.trim();const data=await api('/api/tasks',{method:'POST',body:JSON.stringify(payload)});if(workspaceDirty)clearWorkspaceEditor();$('submitMessage').textContent=`Принято: ${data.task_id}`;$('task').value='';toast(`Задача ${data.task_id} добавлена в очередь`);await refresh();await showDetails(data.task_id)}catch(err){$('submitMessage').textContent=err.message;toast(err.message)}finally{button.disabled=false}});
$('tokenButton').addEventListener('click',()=>{sessionStorage.removeItem('ao_ui_token');token='';token=prompt('Введите новый токен доступа')||'';if(token)sessionStorage.setItem('ao_ui_token',token);refresh().catch(e=>toast(e.message))});
let savedConfigs = null;
let configRestartRequired = false;
let configDirty = false;
let latestModelStatus = null;
function setConfigMessage(message, kind=''){const node=$('configMessage');node.textContent=message;node.className=`config-message ${kind}`;}
function markConfigDirty(){
  configDirty=true;
  $('configState').textContent='Есть несохранённые изменения';$('configState').className='pill warn';
  if($('nodesConfigState')){$('nodesConfigState').textContent='Черновик не сохранён';$('nodesConfigState').className='pill warn';}
  setConfigMessage('Это черновик в браузере; файлы config/ ещё не изменены. Нажмите «Сохранить конфигурации», чтобы проверить и записать его.');
}
function parseEditor(id,label){try{return JSON.parse($(id).value)}catch(e){throw new Error(`${label}: некорректный JSON — ${e.message}`)}}
async function loadConfigs(){
  const data=await api('/api/configs');
  savedConfigs=data;
  configDirty=false;
  configRestartRequired=!!data.restart_required;
  $('mainConfigEditor').value=JSON.stringify(data.main,null,2);
  $('modelsConfigEditor').value=JSON.stringify(data.models,null,2);
  $('agentsConfigEditor').value=JSON.stringify(data.agents,null,2);
  $('configState').textContent=configRestartRequired?'На диске есть неприменённые изменения':'Синхронизировано';
  $('configState').className=configRestartRequired?'pill warn':'pill ok';
  $('nodesConfigState').textContent=configRestartRequired?'Нужен перезапуск':'Синхронизировано';
  $('nodesConfigState').className=configRestartRequired?'pill warn':'pill ok';
  setConfigMessage(configRestartRequired?'Файлы на диске отличаются от загруженной конфигурации процесса. Перезапустите сервер, чтобы применить их.':'Редактор отображает активные JSON-файлы из config/.');
  renderModels(latestModelStatus||{models:[],decision_models:[],nodes:[]});
}
function renderModels(data){
  latestModelStatus=data;
  const items=[...(data.models||[]),...(data.decision_models||[])];
  const nodes=data.nodes||[];
  renderNodes(data);
  $('modelsList').innerHTML=items.length?items.map(m=>{
    const node=nodes.find(n=>n.id===m.node_id);
    const ready=!!m.available&&(!m.managed||m.running);
    const state=m.running?'Процесс запущен':m.available?'Доступен':'Не готов / выключена';
    const cls=ready?'ready':'off';
    const escapedId=esc(m.id);
    const actionButtons=`<button class="secondary small-button" data-model-enable="${m.enabled?'disable':'enable'}" data-model-kind="${esc(m.kind)}" data-model-id="${escapedId}">${m.enabled?'Выключить':'Включить'}</button>`+
      (m.managed?(m.running?`<button class="secondary small-button" data-model-action="stop" data-model-kind="${esc(m.kind)}" data-model-id="${escapedId}" ${(configRestartRequired||configDirty)?'disabled':''}>Остановить процесс</button>`:`<button class="secondary small-button" data-model-action="start" data-model-kind="${esc(m.kind)}" data-model-id="${escapedId}" ${(!m.enabled||configRestartRequired||configDirty)?'disabled':''}>Запустить процесс</button>`):'');
    return `<article class="model-card"><h3>${esc(m.name)}</h3><div class="model-id">${esc(m.kind)} · ${escapedId}</div><div class="model-meta"><span class="model-state">${esc(({general:'Основная',multimodal:'Мультимодальная',voice_interpreter:'Голосовой посредник'})[m.model_role]||'Основная')}</span><span class="model-state">IN: ${esc(m.input_modalities||'text')}</span><span class="model-state">OUT: ${esc(m.output_modalities||'text')}</span><span class="model-state ${cls}">${esc(state)}</span>${m.auto_start?'<span class="model-state">запуск по требованию</span>':''}${m.start_at_boot?'<span class="model-state">запуск при старте</span>':''}<span class="model-state">${m.enabled?'включена':'выключена'}</span></div><div class="model-url">${esc(m.model_name||'')} · ${esc(m.base_url||'')}</div><div class="model-url">API: ${esc(m.api_type||'openai_compatible')} · Узел: ${esc(node?node.name:(m.node_id||'без привязки'))} · ${esc(m.location_type||'—')}</div>${m.last_error?`<div class="model-error">${esc(m.last_error)}</div>`:''}${m.log_path?`<div class="model-url">Лог: ${esc(m.log_path)}</div>`:''}<div class="model-actions">${actionButtons}</div></article>`
  }).join(''):'<p class="empty">В config/models.json пока нет профилей моделей.</p>';
  $('modelDefaults').textContent=`LLM по умолчанию: ${data.default_model_id||'не задана'} · Decision: ${data.decision_model_id||'не задана'}`;
  if(configRestartRequired)$('modelRestartNote').classList.remove('hidden');else $('modelRestartNote').classList.add('hidden');
}
function renderNodes(data){
  const nodes=data.nodes||[];
  const models=[...(data.models||[]),...(data.decision_models||[])];
  $('nodesList').innerHTML=nodes.length?nodes.map(n=>{
    const attached=models.filter(m=>m.node_id===n.id);
    const typeLabel={local:'Локальное устройство',lan:'Локальная сеть',internet:'Интернет'}[n.type]||n.type||'не указан';
    const endpoint=n.endpoint||'Модели используют собственные endpoint/port';
    return `<article class="node-card"><div class="node-head"><h3>${esc(n.name||n.id)}</h3><span class="pill ${n.enabled?'ok':'muted'}">${n.enabled?'Включён':'Выключен'}</span></div><div class="model-id">${esc(n.id)} · ${esc(typeLabel)}</div><div class="model-url">${esc(endpoint)}</div><div class="node-attached">Модели: ${attached.length?attached.map(m=>esc(m.name)).join(', '):'не привязаны'}</div><div class="model-actions"><button class="secondary small-button" data-node-toggle="${esc(n.id)}">${n.enabled?'Выключить узел':'Включить узел'}</button></div></article>`
  }).join(''):'<p class="empty">Добавьте первый Node. Для локальных моделей узел может не иметь общего endpoint.</p>';
}
async function loadModels(){const data=await api('/api/models');renderModels(data)}
async function loadResources(){
  const data=await api('/api/resources');
  $('resourceDecisionModel').textContent=(data.selection_mode==='local_bm25')?'Выбор ресурсов: локальный BM25 · автоиндексация':`Выбор ресурсов: ${data.classification_parallel?'классификация через decision model':'локальный режим'}`;
  const dirs=data.directories||{};
  const indexPaths=[['Resource DB',data.resource_index_path],['Project memory root',data.project_memory_root],['Project DB',data.project_memory_index_path]].filter(([,path])=>path);
  $('resourceDirectories').innerHTML=indexPaths.map(([name,path])=>`<div class="resource-path"><strong>${esc(name)}</strong><code>${esc(path)}</code></div>`).join('')+Object.entries(dirs).map(([name,path])=>`<div class="resource-path"><strong>${esc(name)}</strong><code>${esc(path)}</code></div>`).join('');
  const files=data.files||{};
  const labels={skills:'Skills',system_prompts:'System prompts',tools:'Tools + MCP docs',knowledge:'Knowledge',instructions:'Classifier instructions'};
  $('resourceGroups').innerHTML=Object.keys(labels).map(key=>{const entries=files[key]||[];return `<article class="resource-card"><h3>${esc(labels[key])}<span>${entries.length}</span></h3>${entries.length?`<ul>${entries.map(f=>`<li><code>${esc(f)}</code></li>`).join('')}</ul>`:'<p class="footnote">Нет Markdown-файлов</p>'}</article>`}).join('');
}
async function toggleModelEnabled(kind,id,enabled){
  const label=`${kind==='decision'?'decision-модель':'LLM'} ${id}`;
  if(!confirm(`${enabled?'Включить':'Выключить'} ${label}?`))return;
  try{const result=await api(`/api/models/${kind}/${encodeURIComponent(id)}/${enabled?'enable':'disable'}`,{method:'POST',body:'{}'});if(result.models)renderModels(result.models);await loadModels();await loadConfigs();toast(`${label}: ${enabled?'включена':'выключена'}`)}catch(e){toast(e.message)}
}
function modifyNodeConfig(action,id){
  try{
    const cfg=parseEditor('modelsConfigEditor','Реестр моделей и узлов');
    if(!Array.isArray(cfg.nodes))cfg.nodes=[];
    if(action==='add'){
      const nodeId=(prompt('ID узла:',`node-${cfg.nodes.length+1}`)||'').trim();if(!nodeId)return;
      if(!/^[A-Za-z0-9_-]+$/.test(nodeId)||cfg.nodes.some(n=>n.id===nodeId))throw new Error('Недопустимый или занятый ID узла');
      const type=(prompt('Тип: local, lan или internet','local')||'').trim();if(!['local','lan','internet'].includes(type))throw new Error('Тип узла должен быть local, lan или internet');
      const name=(prompt('Название узла',nodeId)||nodeId).trim();
      const endpoint=type==='local'?'':(prompt('Base endpoint (например https://host/v1 или https://host/v1/systemone):','')||'').trim();
      const api_type=endpoint?(prompt('Контракт API: openai_compatible или systemone','openai_compatible')||'openai_compatible').trim():'openai_compatible';
      if(!['openai_compatible','systemone'].includes(api_type))throw new Error('api_type должен быть openai_compatible или systemone');
      const health_url=type==='local'?'':(prompt('URL проверки готовности (необязательно):',endpoint?`${endpoint.replace(/\/$/,'')}/models`:'')||'').trim();
      const api_key_env=(type==='internet'||type==='lan')?(prompt('Имя переменной окружения для API key (секрет не будет сохранён в JSON):','ASGEND_REMOTE_LLM_API_KEY')||'').trim():'';
      cfg.nodes.push({id:nodeId,name,type,enabled:type==='local',endpoint,health_url,api_type,api_key:'',api_key_env,description:''});
    }else{
      const node=cfg.nodes.find(n=>n.id===id);if(!node)throw new Error(`Узел ${id} не найден`);
      node.enabled=!node.enabled;
      if(!node.enabled){const linked=[...(cfg.models||[]),...(cfg.decision_models||[])].filter(m=>m.node_id===id);if(linked.length&&!confirm(`Узел используется ${linked.length} моделью. Они будут недоступны после применения конфигурации. Продолжить?`)){node.enabled=true;return;}}
    }
    $('modelsConfigEditor').value=JSON.stringify(cfg,null,2);markConfigDirty();renderNodes({...latestModelStatus,nodes:cfg.nodes});
  }catch(e){toast(e.message);setConfigMessage(e.message,'error')}
}
async function modelAction(kind,id,action){
  if(configRestartRequired||configDirty){toast('Сначала сохраните или перезагрузите изменения конфигурации');return;}
  const label=`${kind==='decision'?'decision-модель':'LLM'} ${id}`;
  if(action==='stop'&&!confirm(`Остановить ${label}?`))return;
  try{const result=await api(`/api/models/${kind}/${encodeURIComponent(id)}/${action}`,{method:'POST',body:'{}'});if(result.models)renderModels(result.models);await loadModels();toast(action==='start'?`${label}: запуск завершён`:`${label}: остановлена`)}catch(e){toast(e.message);await loadModels().catch(()=>{})}
}
function readAllEditors(){
  const main=parseEditor('mainConfigEditor','Основная конфигурация');
  const models=parseEditor('modelsConfigEditor','Конфигурация моделей');
  const agents=parseEditor('agentsConfigEditor','Конфигурация профилей исполнения');
  if(!Array.isArray(agents))throw new Error('Конфигурация профилей должна быть JSON-массивом');
  if(!Array.isArray(models.models)||!Array.isArray(models.decision_models))throw new Error('В models.json должны быть массивы models и decision_models');
  return {main,models,agents};
}
async function saveConfigs(){
  const button=$('saveConfigs');
  const editorControls=[...document.querySelectorAll('#mainConfigEditor,#modelsConfigEditor,#agentsConfigEditor,#saveConfigs,#saveConfigsAgents,#reloadConfigs')];
  editorControls.forEach(control=>control.disabled=true);
  try{
    const payload=readAllEditors();
    const result=await api('/api/configs',{method:'PUT',body:JSON.stringify(payload)});
    configRestartRequired=!!result.restart_required;
    configDirty=false;
    $('configState').textContent=configRestartRequired?'Сохранено · нужен перезапуск':'Синхронизировано';$('configState').className=configRestartRequired?'pill warn':'pill ok';
    setConfigMessage(configRestartRequired?'Конфигурация сохранена в файлы на диске. Изменения будут загружены после перезапуска сервера.':'Конфигурация уже соответствует активному состоянию.','success');
    renderModels(latestModelStatus||{models:[],decision_models:[]});
    toast(configRestartRequired?'Конфигурация сохранена; перезапустите сервер для применения':'Конфигурация сохранена');
  }catch(e){setConfigMessage(e.message,'error');toast(e.message)}finally{editorControls.forEach(control=>control.disabled=false);button.disabled=false}
}
function modifyAgents(action){
  try{
    const agents=parseEditor('agentsConfigEditor','Конфигурация профилей исполнения');
    if(!Array.isArray(agents))throw new Error('Ожидается JSON-массив профилей');
    if(action==='add'){
      const id=(prompt('ID нового профиля (латиница, цифры, _ или -):','new-profile')||'').trim();
      if(!id)return;
      if(!/^[A-Za-z0-9_-]+$/.test(id)||agents.some(a=>a.id===id))throw new Error('Недопустимый или уже существующий ID');
      agents.push({id,name:id,description:'',system_prompt:'You are a helpful task execution agent. Follow the task instruction and return a concise, verifiable result.',model_id:'',skills:[],tools:[]});
    }else{
      const ids=agents.map(a=>a.id).join(', ');
      const id=(prompt(`ID профиля для удаления. Доступны: ${ids}`,'')||'').trim();if(!id)return;
      if(!agents.some(a=>a.id===id))throw new Error(`Профиль ${id} не найден`);
      if(!confirm(`Удалить профиль ${id}?`))return;
      for(let i=agents.length-1;i>=0;i--)if(agents[i].id===id)agents.splice(i,1);
      if(!agents.length)throw new Error('Нельзя удалить последний профиль');
    }
    $('agentsConfigEditor').value=JSON.stringify(agents,null,2);markConfigDirty();
  }catch(e){toast(e.message);setConfigMessage(e.message,'error')}
}
let modelDialogDecision=false;
const MODEL_FORM_FIELDS=[
  {key:'id',label:'ID модели',type:'text',required:true,pattern:'[A-Za-z0-9_-]+'},
  {key:'name',label:'Название',type:'text',required:true},
  {key:'enabled',label:'Включена',type:'checkbox'},
  {key:'auto_start',label:'Запуск по требованию',type:'checkbox'},
  {key:'start_at_boot',label:'Запуск при старте ASGEND',type:'checkbox'},
  {key:'executable',label:'Путь к исполняемому файлу (пусто для внешнего endpoint)',type:'text',wide:true},
  {key:'model_path',label:'Путь к файлу модели',type:'text',wide:true},
  {key:'working_directory',label:'Рабочий каталог процесса',type:'text',wide:true},
  {key:'args',label:'Аргументы запуска · один аргумент на строку',type:'textarea',wide:true},
  {key:'port',label:'Порт',type:'number',min:0,max:65535},
  {key:'model_name',label:'Имя модели для API',type:'text',required:true},
  {key:'base_url',label:'Base URL / API endpoint',type:'url',required:true,wide:true},
  {key:'api_type',label:'Тип API',type:'select',options:[['openai_compatible','OpenAI-compatible'],['systemone','System One']]},
  {key:'model_role',label:'Роль модели',type:'select',options:[['general','Основная LLM'],['multimodal','Мультимодальная'],['voice_interpreter','Голосовой посредник']],wide:true},
  {key:'runtime_adapter',label:'Адаптер runtime',type:'select',options:[['openai_compatible','OpenAI-compatible endpoint'],['llama_server_openai_compatible','LFM3-Omni · llama-server']],wide:true},
  {key:'input_modalities',label:'Входные модальности (через запятую)',type:'text'},
  {key:'output_modalities',label:'Выходные модальности (через запятую)',type:'text'},
  {key:'health_url',label:'URL проверки готовности',type:'url'},
  {key:'api_key',label:'API-ключ',type:'password'},
  {key:'node_id',label:'Node ID',type:'node'},
  {key:'location_type',label:'Тип размещения',type:'select',options:[['local','Локально'],['lan','Локальная сеть'],['internet','Интернет']]},
  {key:'tier',label:'Уровень модели (tier)',type:'text'},
  {key:'ram_estimate_mb',label:'Оценка RAM, МиБ',type:'number',min:0,max:1048576},
  {key:'vram_estimate_mb',label:'Оценка VRAM, МиБ',type:'number',min:0,max:1048576},
  {key:'startup_timeout_seconds',label:'Тайм-аут запуска, с',type:'number',min:1,max:900},
  {key:'shutdown_timeout_seconds',label:'Тайм-аут остановки, с',type:'number',min:1,max:60},
  {key:'request_timeout_seconds',label:'Общий тайм-аут запроса, с',type:'number',min:1,max:900},
  {key:'first_token_timeout_seconds',label:'Тайм-аут первого токена, с',type:'number',min:1,max:900},
  {key:'stream_idle_timeout_seconds',label:'Тайм-аут простоя потока, с',type:'number',min:1,max:900},
  {key:'total_timeout_seconds',label:'Жёсткий лимит генерации, с (0 = без лимита)',type:'number',min:0,max:3600},
  {key:'temperature',label:'Temperature',type:'number',min:0,max:2,step:0.1}
];
function openModelDialog(decision){
  try{
    const cfg=parseEditor('modelsConfigEditor','Конфигурация моделей');
    if(!Array.isArray(cfg.models))cfg.models=[];if(!Array.isArray(cfg.decision_models))cfg.decision_models=[];if(!Array.isArray(cfg.nodes))cfg.nodes=[];
    modelDialogDecision=decision;$('modelDialogTitle').textContent=decision?'Добавить decision-модель':'Добавить LLM';
    $('modelFormSave').textContent='Добавить в конфигурацию';
    const nodeChoices=[['','Без привязки'],...cfg.nodes.map(n=>[n.id,`${n.name||n.id} · ${n.type||'local'}`])];
    const def={id:decision?`new-router-${cfg.decision_models.length+1}`:`new-llm-${cfg.models.length+1}`,name:'',enabled:false,auto_start:false,start_at_boot:false,executable:'',model_path:'',working_directory:'.',args:[],port:8100+cfg.models.length+cfg.decision_models.length,base_url:'http://127.0.0.1:8100/v1',api_type:decision?'systemone':'openai_compatible',health_url:'',model_name:'',api_key:'',node_id:'',location_type:'local',tier:decision?'decision':'medium',ram_estimate_mb:0,vram_estimate_mb:0,startup_timeout_seconds:120,shutdown_timeout_seconds:10,request_timeout_seconds:180,first_token_timeout_seconds:180,stream_idle_timeout_seconds:180,total_timeout_seconds:0,temperature:0.2,model_role:'general',runtime_adapter:'openai_compatible',input_modalities:'text',output_modalities:'text'};
    const host=$('modelFormFields');host.innerHTML='';
    for(const field of MODEL_FORM_FIELDS){
      const wrap=document.createElement('div');wrap.className=`model-form-field${field.wide?' wide':''}${field.type==='checkbox'?' checkbox':''}`;
      const label=document.createElement('label');label.htmlFor=`model-field-${field.key}`;label.textContent=field.label;
      let input;
      if(field.type==='textarea'){input=document.createElement('textarea');input.value=field.key==='args'?(def.args||[]).join('\n'):'';}
      else if(field.type==='select'||field.type==='node'){
        input=document.createElement('select');const options=field.type==='node'?nodeChoices:field.options;
        for(const [value,text] of options){const option=document.createElement('option');option.value=value;option.textContent=text;input.appendChild(option)}
      }else{input=document.createElement('input');input.type=field.type;if(field.type==='checkbox')input.checked=!!def[field.key];else input.value=String(def[field.key]??'');if(field.min!==undefined)input.min=String(field.min);if(field.max!==undefined)input.max=String(field.max);if(field.step!==undefined)input.step=String(field.step);if(field.pattern)input.pattern=field.pattern;}
      input.id=`model-field-${field.key}`;input.name=field.key;if(field.required)input.required=true;
      if(field.type==='select'&&def[field.key]!==undefined)input.value=def[field.key];
      if(field.key==='name')input.value=def.id;
      if(field.key==='model_name')input.value=def.id;
      if(field.key==='node_id')input.value=nodeChoices[0]?.[0]||'';
      if(field.key==='health_url')input.placeholder='Например http://127.0.0.1:8100/health';
      if(field.key==='base_url')input.value=def.base_url;
      if(field.key==='runtime_adapter')input.addEventListener('change',()=>{if(input.value==='llama_server_openai_compatible'){const set=(key,value)=>{const target=$(`model-field-${key}`);if(target)target.value=value};set('id','lfm3-omni-2b');set('name','LFM3-Omni-2B · voice interpreter');set('model_name','LiquidAI/LFM3-Omni-2B-GGUF:Q4_K_M');set('model_role','voice_interpreter');set('input_modalities','text,audio,image,video');set('output_modalities','text,audio');set('base_url','http://127.0.0.1:8787/v1');set('health_url','http://127.0.0.1:8787/health');set('port','8787');set('enabled',false);set('auto_start',false);set('start_at_boot',false);const apiType=$('model-field-api_type');if(apiType)apiType.value='openai_compatible';}});
      wrap.append(label,input);
      if(field.key==='args'){const note=document.createElement('small');note.className='footnote';note.textContent='Для llama-server можно оставить список пустым: core автоматически добавит модель, адрес и порт.';wrap.appendChild(note)}
      host.appendChild(wrap);
    }
    $('modelDialog').showModal();
  }catch(e){toast(e.message);setConfigMessage(e.message,'error')}
}
function addModelConfig(decision){openModelDialog(decision)}
$('modelFormCancel').addEventListener('click',()=>$('modelDialog').close());
$('modelDialogForm').addEventListener('submit',e=>{
  e.preventDefault();
  try{
    const cfg=parseEditor('modelsConfigEditor','Конфигурация моделей');if(!Array.isArray(cfg.models))cfg.models=[];if(!Array.isArray(cfg.decision_models))cfg.decision_models=[];
    const model={};
    for(const field of MODEL_FORM_FIELDS){const el=$(`model-field-${field.key}`);if(!el)continue;
      if(field.type==='checkbox')model[field.key]=el.checked;
      else if(field.key==='args')model.args=el.value.split(/\r?\n/).map(x=>x.trim()).filter(Boolean);
      else if(field.type==='number')model[field.key]=Number(el.value);
      else model[field.key]=el.value.trim();
    }
    if(!/^[A-Za-z0-9_-]+$/.test(model.id))throw new Error('ID должен содержать латинские буквы, цифры, дефис или подчёркивание');
    if([...cfg.models,...cfg.decision_models].some(m=>m.id===model.id))throw new Error(`Модель ${model.id} уже существует`);
    if(!model.name||!model.base_url||!model.model_name)throw new Error('Заполните название, Base URL и имя модели для API');
    const allowedInput=['text','audio','image','video'],allowedOutput=['text','audio'];
    const validCsv=(value,allowed)=>{const parts=String(value||'').split(',').map(x=>x.trim()).filter(Boolean);return parts.length>0&&new Set(parts).size===parts.length&&parts.every(x=>allowed.includes(x))};
    if(!validCsv(model.input_modalities,allowedInput)||!validCsv(model.output_modalities,allowedOutput))throw new Error('Укажите поддерживаемые модальности через запятую: input — text,audio,image,video; output — text,audio');
    if(model.runtime_adapter==='llama_server_openai_compatible'&&(model.model_role!=='voice_interpreter'||!model.input_modalities.split(',').map(x=>x.trim()).includes('audio')||!model.output_modalities.split(',').map(x=>x.trim()).includes('text')))throw new Error('LFM3-Omni требует роль voice_interpreter, audio на входе и text на выходе');
    if(model.location_type==='internet'&&!model.base_url.startsWith('https://'))throw new Error('Для интернет-узла используйте HTTPS');
    const node=Array.isArray(cfg.nodes)?cfg.nodes.find(n=>n.id===model.node_id):null;
    if(node){model.location_type=node.type||model.location_type;if(node.endpoint)model.base_url=node.endpoint;if(node.health_url)model.health_url=node.health_url;if(node.api_type)model.api_type=node.api_type;}
    (model.node_id?null:null);
    (model.node_id=== '' ? delete model.node_id : null);
    (model.location_type=== '' ? delete model.location_type : null);
    (model.api_key=== '' ? model.api_key='' : null);
    (model.executable=== '' ? model.executable='' : null);
    (model.model_path=== '' ? model.model_path='' : null);
    const target=modelDialogDecision?cfg.decision_models:cfg.models;target.push(model);
    $('modelsConfigEditor').value=JSON.stringify(cfg,null,2);markConfigDirty();renderModels({...latestModelStatus,models:cfg.models,decision_models:cfg.decision_models,nodes:cfg.nodes});$('modelDialog').close();
    setConfigMessage('Модель добавлена в черновик. Нажмите «Сохранить конфигурации», чтобы проверить и записать реестр.');toast('Профиль модели добавлен');
  }catch(err){toast(err.message)}
});
$('modelsList').addEventListener('click',e=>{const enabled=e.target.closest('[data-model-enable]');if(enabled){toggleModelEnabled(enabled.dataset.modelKind,enabled.dataset.modelId,enabled.dataset.modelEnable==='enable');return;}const b=e.target.closest('[data-model-action]');if(b)modelAction(b.dataset.modelKind,b.dataset.modelId,b.dataset.modelAction)});
$('nodesList').addEventListener('click',e=>{const b=e.target.closest('[data-node-toggle]');if(b)modifyNodeConfig('toggle',b.dataset.nodeToggle)});
for(const editorId of ['mainConfigEditor','modelsConfigEditor','agentsConfigEditor'])$(editorId).addEventListener('input',markConfigDirty);
$('addNode').addEventListener('click',()=>modifyNodeConfig('add'));
$('refreshNodes').addEventListener('click',()=>loadModels().catch(e=>toast(e.message)));
$('refreshResources').addEventListener('click',()=>loadResources().catch(e=>toast(e.message)));
$('refreshModels').addEventListener('click',()=>loadModels().catch(e=>toast(e.message)));
$('saveConfigs').addEventListener('click',saveConfigs);
$('saveConfigsAgents').addEventListener('click',saveConfigs);
$('reloadConfigs').addEventListener('click',()=>{if(confirm('Заменить все изменения в редакторах содержимым файлов на диске?'))loadConfigs().catch(e=>toast(e.message))});
$('addAgent').addEventListener('click',()=>modifyAgents('add'));
$('removeAgent').addEventListener('click',()=>modifyAgents('remove'));
$('addModel').addEventListener('click',()=>addModelConfig(false));
$('addDecisionModel').addEventListener('click',()=>addModelConfig(true));

// Cowork voice interpreter: a separate LFM3 gateway builds a revisioned report;
// the ASGEND task queue is contacted only by the explicit confirm action.
const COWORK_GATEWAY = 'http://127.0.0.1:8788';
if('serviceWorker' in navigator){window.addEventListener('load',()=>navigator.serviceWorker.register('/sw.js').catch(err=>console.warn('PWA service worker unavailable:',err)));}
let coworkSessionId = '';
let coworkRevision = 0;
let coworkPendingSelections = [];
let coworkRecorder = null;
let coworkRecordStream = null;
let coworkRecordedChunks = [];
let coworkMediaPending = [];
let coworkProjectPath = '';
let coworkProjectEntries = [];
let coworkProjectSelectedPath = '';
let coworkProjectOriginal = '';
let coworkProjectDirty = false;
let coworkProjectPane = 'office';
let coworkProjectObjectUrl = null;
function coworkAddMessage(who, text){
  const host=$('coworkDialogue');if(!host)return;
  const empty=host.querySelector('.empty');if(empty)empty.remove();
  const item=document.createElement('div');item.className=`cowork-message ${who==='Вы'?'user':'assistant'}`;
  const label=document.createElement('strong');label.textContent=who;
  const body=document.createElement('div');body.textContent=text;
  item.append(label,body);host.appendChild(item);host.scrollTop=host.scrollHeight;
}
async function coworkApi(path, body){
  await ensureToken();if(!token)throw new Error('Сначала введите токен Control Plane');
  const response=await fetch(COWORK_GATEWAY+path,{method:'POST',headers:{'Content-Type':'application/json','X-AO-Token':token},body:JSON.stringify(body),cache:'no-store'});
  const data=await response.json().catch(()=>({ok:false,error:'Голосовой шлюз вернул некорректный ответ'}));
  if(!response.ok||data.ok===false)throw new Error(data.error||`Voice gateway HTTP ${response.status}`);
  return data;
}
function coworkProjectContext(){
  const parts=[];
  if(currentTaskId)parts.push(`ASGEND task ID: ${currentTaskId}`);
  if($('chatTaskTitle')?.textContent)parts.push(`Текущая задача: ${$('chatTaskTitle').textContent}`);
  if($('workspacePath')?.textContent)parts.push($('workspacePath').textContent);
  if(workspaceSelectedFile)parts.push(`Открытый файл: ${workspaceSelectedFile}`);
  if(workspaceDirty)parts.push('В существующем редакторе есть несохранённые изменения. Не считать их сохранёнными в проекте.');
  return parts.join('\n');
}
async function coworkProjectState(){
  const parts=[coworkProjectContext()];
  if(currentTaskId){
    try{const task=await api('/api/tasks/'+encodeURIComponent(currentTaskId));parts.push(`Статус задачи: ${task.status||'unknown'}`);if(task.title||task.task)parts.push(`Задание основной модели: ${task.title||task.task}`);if(task.error)parts.push(`Последняя ошибка: ${task.error}`);}
    catch(err){parts.push(`Состояние задачи недоступно: ${err.message}`)}
    try{const files=await workspaceRequest('artifact_list',{},currentTaskId);const rows=(files.artifacts||[]).slice(0,120).map(x=>`${x.path} (${x.mime||'file'}, ${formatFileSize(x.size)})`);parts.push('Файлы рабочей папки:\n'+rows.join('\n'));if(files.truncated)parts.push('Список файлов частичный (применены лимиты безопасности).');}
    catch(err){parts.push(`Список файлов недоступен: ${err.message}`)}
  }
  if(workspaceSelectedFile)parts.push(`Открытый текстовый файл ${workspaceSelectedFile}:\n${String($('workspaceFileEditor')?.value||'').slice(0,30000)}`);
  if(coworkProjectSelectedPath&&$('coworkProjectEditor')&&!$('coworkProjectEditor').classList.contains('hidden'))parts.push(`Открытый файл ${coworkProjectSelectedPath}:\n${String($('coworkProjectEditor').value||'').slice(0,30000)}`);
  return parts.filter(Boolean).join('\n\n').slice(0,100000);
}
async function coworkEnsureSession(){
  if(coworkSessionId)return coworkSessionId;
  await ensureToken();if(!token)throw new Error('Сначала введите токен Control Plane');
  const response=await fetch(COWORK_GATEWAY+'/v1/sessions',{method:'POST',headers:{'Content-Type':'application/json','X-AO-Token':token},body:JSON.stringify({project_state:await coworkProjectState()}),cache:'no-store'});
  const data=await response.json().catch(()=>({ok:false,error:'Не удалось создать сессию Cowork'}));
  if(!response.ok||!data.ok)throw new Error(data.error||`Voice gateway HTTP ${response.status}`);
  coworkSessionId=data.session_id;$('coworkSessionState').textContent='Сессия активна';$('coworkSessionState').className='pill ok';
  return coworkSessionId;
}
async function fileToCoworkAttachment(file){
  if(file.size>24*1024*1024)throw new Error(`${file.name}: размер больше 24 МиБ`);
  const dataUrl=await new Promise((resolve,reject)=>{const reader=new FileReader();reader.onload=()=>resolve(String(reader.result));reader.onerror=()=>reject(new Error(`Не удалось прочитать ${file.name}`));reader.readAsDataURL(file)});
  const mime=file.type||({wav:'audio/wav',mp3:'audio/mpeg',mp4:'video/mp4'}[file.name.split('.').pop().toLowerCase()]||'application/octet-stream');
  return {name:file.name,mime_type:mime,data_base64:dataUrl.split(',',2)[1]||''};
}
function coworkCaptureSelection(){
  const active=document.activeElement;
  if(active&&typeof active.selectionStart==='number'&&typeof active.selectionEnd==='number'&&active.selectionEnd>active.selectionStart){
    const text=active.value.slice(active.selectionStart,active.selectionEnd);
    const label=active.id==='workspaceFileEditor'?(workspaceSelectedFile||'редактор файла'):active.id==='coworkProjectEditor'?(coworkProjectSelectedPath||'файл проекта'):(active.getAttribute('aria-label')||active.id||'текстовое поле');
    return {kind:/\.(c|h|cpp|js|ts|py|go|rs|java|kt|dart|css|html|json|yaml|yml)$/i.test(label)?'code':'text',label,text};
  }
  const selection=window.getSelection();
  if(selection&&String(selection).trim()){
    const text=String(selection).trim();const node=selection.anchorNode?.parentElement;
    const label=node?.closest('[data-file-path]')?.getAttribute('data-file-path')||node?.closest('pre,code')?.getAttribute('data-language')||node?.closest('h1,h2,h3')?.textContent||'выделенный объект';
    return {kind:node?.closest('pre,code')?'code':'text',label:String(label||'выделенный объект').slice(0,180),text};
  }
  return null;
}
function coworkRefreshSelectionStatus(){
  const candidate=coworkCaptureSelection();
  $('coworkSelectionStatus').textContent=candidate?`Выделено: ${candidate.label} · ${candidate.text.length} симв.`:(coworkPendingSelections.length?`В отчёте ожидают отправки: ${coworkPendingSelections.length} выделений`:'Ничего не выделено. Можно описать объект словами.');
}
document.addEventListener('mouseup',coworkRefreshSelectionStatus);
document.addEventListener('keyup',coworkRefreshSelectionStatus);
$('coworkAddSelection').addEventListener('mousedown',e=>e.preventDefault());
$('coworkAddSelection').addEventListener('click',()=>{
  const selected=coworkCaptureSelection();
  if(!selected){toast('Выделите текст, код или объект либо опишите его в сообщении');return}
  if(coworkPendingSelections.length>=32){toast('Максимум 32 выделения за один ход');return}
  coworkPendingSelections.push(selected);$('coworkSelectionStatus').textContent=`Добавлено: ${selected.label} · ${coworkPendingSelections.length} шт.`;
  coworkAddMessage('Контекст',`Добавлено выделение «${selected.label}» (${selected.text.length} символов). Оно будет учтено при следующем обновлении отчёта.`);
});
$('coworkMediaFiles').addEventListener('change',async e=>{
  try{for(const file of Array.from(e.target.files||[])){coworkMediaPending.push(await fileToCoworkAttachment(file))} $('coworkComposeStatus').textContent=`Подготовлено вложений: ${coworkMediaPending.length}`}
  catch(err){toast(err.message)}finally{e.target.value=''}
});
$('coworkRecord').addEventListener('click',async()=>{
  if(coworkRecorder&&coworkRecorder.state==='recording'){coworkRecorder.stop();$('coworkRecord').textContent='🎙 Начать запись';$('coworkComposeStatus').textContent='Обработка записи…';return}
  if(!navigator.mediaDevices?.getUserMedia||!window.MediaRecorder){toast('Запись аудио не поддерживается этим браузером. Прикрепите аудиофайл.');return}
  try{
    coworkRecordStream=await navigator.mediaDevices.getUserMedia({audio:true});
    const preferred=['audio/webm;codecs=opus','audio/mp4','audio/webm'].find(x=>MediaRecorder.isTypeSupported?.(x));
    coworkRecorder=new MediaRecorder(coworkRecordStream,preferred?{mimeType:preferred}:undefined);coworkRecordedChunks=[];
    coworkRecorder.ondataavailable=e=>{if(e.data&&e.data.size)coworkRecordedChunks.push(e.data)};
    coworkRecorder.onstop=async()=>{
      try{const blob=new Blob(coworkRecordedChunks,{type:coworkRecorder.mimeType||'audio/webm'});const ext=blob.type.includes('mp4')?'m4a':blob.type.includes('ogg')?'ogg':'webm';
        const file=new File([blob],`cowork-voice-${Date.now()}.${ext}`,{type:blob.type});coworkMediaPending.push(await fileToCoworkAttachment(file));
        $('coworkComposeStatus').textContent=`Голосовая запись подготовлена (${formatFileSize(blob.size)}). Нажмите «Обновить отчёт».`;
      }catch(err){toast(err.message)}finally{coworkRecordStream?.getTracks().forEach(t=>t.stop());coworkRecordStream=null;}
    };
    coworkRecorder.start();$('coworkRecord').textContent='■ Остановить запись';$('coworkComposeStatus').textContent='Идёт запись. Нажмите «Остановить запись», когда закончите.';
  }catch(err){toast(`Нет доступа к микрофону: ${err.message}`)}
});
$('coworkForm').addEventListener('submit',async e=>{
  e.preventDefault();const text=$('coworkText').value.trim();
  if(!text&&!coworkPendingSelections.length&&!coworkMediaPending.length){toast('Добавьте текст, выделение или аудио/изображение/видео');return}
  const button=$('coworkSendTurn');button.disabled=true;$('coworkComposeStatus').textContent='LFM3 анализирует сообщение и обновляет отчёт…';
  try{
    const sid=await coworkEnsureSession();const attachments=coworkMediaPending.slice();
    const data=await coworkApi(`/v1/sessions/${encodeURIComponent(sid)}/turn`,{text,project_state:await coworkProjectState(),selections:coworkPendingSelections.slice(),attachments});
    if(data.revision<=coworkRevision)throw new Error('Получена устаревшая ревизия отчёта');
    coworkRevision=data.revision;coworkAddMessage('Вы',text||'Переданы выделение или мультимедийное вложение');
    $('coworkReport').innerHTML=markdownHtml(data.report);$('coworkReportRevision').textContent=`v${data.revision}`;$('coworkReportTitle').textContent='Отчёт готов к проверке';$('coworkReportPanel').classList.remove('hidden');$('coworkConfirm').disabled=false;
    $('coworkText').value='';coworkPendingSelections=[];coworkMediaPending=[];$('coworkComposeStatus').textContent=`Отчёт обновлён. Выделения: ${data.selection_count}; медиа: ${data.attachment_count}. Проверьте отчёт или добавьте уточнение.`;coworkRefreshSelectionStatus();
    if(data.speech_wav_base64){const audio=$('coworkSpeechReply');audio.src=`data:audio/wav;base64,${data.speech_wav_base64}`;audio.hidden=false;audio.play().catch(()=>{});}else{$('coworkSpeechReply').hidden=true;}
  }catch(err){$('coworkComposeStatus').textContent=err.message;toast(err.message)}finally{button.disabled=false}
});
$('coworkConfirm').addEventListener('click',async()=>{
  if(!coworkSessionId||!coworkRevision)return;const button=$('coworkConfirm');
  if(!confirm(`Подтвердить отчёт версии ${coworkRevision} и отправить его в главную очередь ASGEND?`))return;
  button.disabled=true;button.textContent='Отправляем…';
  try{const result=await coworkApi(`/v1/sessions/${encodeURIComponent(coworkSessionId)}/confirm`,{revision:coworkRevision});
    button.textContent=`Создана задача ${result.task_id}`;$('coworkSessionState').textContent='Отправлено в очередь';$('coworkSessionState').className='pill ok';
    coworkAddMessage('ASGEND',`Подтверждённый отчёт v${result.revision} принят основной очередью. ID задачи: ${result.task_id}`);toast(`Создана задача ${result.task_id}`);await refresh();
    currentTaskId=result.task_id;activateView('chat');await showDetails(result.task_id);
  }catch(err){button.disabled=false;button.textContent='Подтвердить и отправить';toast(err.message)}
});

function renderCoworkProjectFiles(data){
  coworkProjectEntries=data.entries||[];$('coworkProjectPath').textContent=data.path?`/${data.path}`:'/';
  const host=$('coworkProjectFiles');if(!coworkProjectEntries.length){host.innerHTML='<p class="empty">В этой папке нет файлов.</p>';return}
  host.innerHTML=coworkProjectEntries.map(item=>{const path=[data.path,item.name].filter(Boolean).join('/');return `<button type="button" class="cowork-project-file${coworkProjectSelectedPath===path?' active':''}" data-cowork-file="${esc(path)}"><span>${item.kind==='directory'?'📁':'📄'}</span><span title="${esc(item.name)}">${esc(item.name)}</span></button>`}).join('');
}
async function loadCoworkProjectFiles(path=''){
  if(!currentTaskId){$('coworkProjectFiles').innerHTML='<p class="empty">Сначала выберите задачу во вкладке «Рабочее пространство».</p>';return}
  try{
    const data=await workspaceRequest('artifact_list',{},currentTaskId);
    coworkProjectPath='';coworkProjectEntries=(data.artifacts||[]).map(item=>({name:item.path,path:item.path,size:item.size,mime:item.mime,kind:'file'}));
    renderCoworkProjectFiles({path:'',entries:coworkProjectEntries});$('coworkProjectLabel').textContent=`Задача: ${currentTaskId} · файлов: ${coworkProjectEntries.length}`;
  }catch(err){$('coworkProjectFiles').innerHTML=`<p class="empty">${esc(err.message)}</p>`}
}
async function readCoworkBinary(path, size){
  const chunks=[];let offset=0;const maxBytes=16*1024*1024;const fullSize=Number(size)||0;
  if(fullSize>maxBytes)throw new Error('Предпросмотр ограничен 16 МиБ; файл остаётся доступен агенту в рабочей папке.');
  while(offset<fullSize){const data=await workspaceRequest('read_bytes',{path,offset,limit:Math.min(1024*1024,fullSize-offset)},currentTaskId);const raw=atob(data.data_b64||'');const bytes=new Uint8Array(raw.length);for(let i=0;i<raw.length;i++)bytes[i]=raw.charCodeAt(i);chunks.push(bytes);const next=Number(data.next_offset);if(!Number.isFinite(next)||next<=offset)throw new Error('Некорректный файловый блок от Control Plane');offset=next;}
  return new Blob(chunks,{type:coworkProjectEntries.find(x=>x.path===path)?.mime||'application/octet-stream'});
}

function setCoworkPreviewPane(pane){coworkProjectPane=pane;document.querySelectorAll('[data-cowork-pane]').forEach(b=>b.classList.toggle('active',b.dataset.coworkPane===pane));if(coworkProjectSelectedPath){const e=coworkProjectEntries.find(x=>x.path===coworkProjectSelectedPath);if(e&&!((e.mime||'').startsWith('text/')||/\.(md|txt|json|js|ts|tsx|jsx|css|html?|xml|ya?ml|toml|py|c|h|cpp|hpp|rs|go|java|kt|dart|sh|sql|svg)$/i.test(e.path)))openCoworkProjectFile(coworkProjectSelectedPath).catch(err=>toast(err.message));else renderCoworkPreview()}else renderCoworkPreview()}
function renderCoworkPreview(){
  const path=coworkProjectSelectedPath,ext=(path.split('.').pop()||'').toLowerCase(),host=$('coworkPreview');
  $('coworkPreviewTitle').textContent=path||'Предпросмотр';$('coworkPreviewMeta').textContent=path?`${coworkProjectPane.toUpperCase()} · ${formatFileSize(new Blob([coworkProjectOriginal]).size)}`:'—';
  if(!path){host.innerHTML='<p class="empty">Выберите файл проекта.</p>';return}
  const editorContent=$('coworkProjectEditor')&&!$('coworkProjectEditor').classList.contains('hidden')?$('coworkProjectEditor').value:coworkProjectOriginal;
  const isHtml=['html','htm'].includes(ext);
  if(isHtml&&coworkProjectPane!=='code'){
const frame=document.createElement('iframe');frame.setAttribute('sandbox','');frame.title='Изолированный предпросмотр проекта';
    const safeHtml=/\.html?$/i.test(path)?editorContent:`<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><style>body{font:16px system-ui;padding:20px;color:#222}</style><h2>${esc(path)}</h2><p>Выбран режим ${esc(coworkProjectPane)}. Откройте HTML-файл в списке проекта для предпросмотра интерфейса.</p>`;
    frame.srcdoc=`<!doctype html><meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src data: blob:; style-src 'unsafe-inline';"><meta charset="utf-8">${safeHtml}`;host.replaceChildren(frame);return;}
  if(['png','jpg','jpeg','webp','gif','svg'].includes(ext)){host.innerHTML='<p class="empty">Изображения проекта доступны через медиапредпросмотр. Для SVG открыт только текстовый код.</p>';return}
  if(['pdf','docx','xlsx','pptx','mpp'].includes(ext)){host.innerHTML=`<p class="empty">${esc(ext.toUpperCase())}: этот тип не поддерживается текстовым RPC-просмотром текущей версии. Файл остаётся доступен агенту в рабочей папке. Для просмотра используйте Office/PDF-приложение на устройстве.</p>`;return}
  if(['site','ui','mobile'].includes(coworkProjectPane)&&!isHtml){host.innerHTML='<p class="empty">Для предпросмотра сайта или мобильного интерфейса откройте HTML-файл проекта.</p>';return;}
  host.innerHTML='';const pre=document.createElement('pre');pre.textContent=editorContent;host.appendChild(pre);
}
async function openCoworkProjectFile(path){
  const entry=coworkProjectEntries.find(x=>x.path===path||x.name===path)||null;
  if(!entry)return;
  coworkProjectSelectedPath=path;$('coworkPreviewTitle').textContent=path;$('coworkPreviewMeta').textContent=`${entry.mime||'file'} · ${formatFileSize(entry.size)}`;
  renderCoworkProjectFiles({path:'',entries:coworkProjectEntries});
  if(coworkProjectObjectUrl){URL.revokeObjectURL(coworkProjectObjectUrl);coworkProjectObjectUrl=null;}
  const mime=entry.mime||'application/octet-stream';
  const textLike=mime.startsWith('text/')||/\.(md|txt|json|js|ts|tsx|jsx|css|html?|xml|ya?ml|toml|py|c|h|cpp|hpp|rs|go|java|kt|dart|sh|sql|svg)$/i.test(path);
  if(textLike){
    try{const data=await workspaceRequest('read',{path},currentTaskId);coworkProjectOriginal=data.content||'';$('coworkProjectEditor').value=coworkProjectOriginal;$('coworkProjectEditor').classList.remove('hidden');$('coworkSaveProjectFile').disabled=true;$('coworkAttachProjectSelection').disabled=false;renderCoworkPreview();return;}
    catch(err){$('coworkProjectEditor').classList.add('hidden');$('coworkSaveProjectFile').disabled=true;$('coworkAttachProjectSelection').disabled=true;$('coworkPreview').textContent=err.message;return;}
  }
  $('coworkProjectEditor').classList.add('hidden');$('coworkSaveProjectFile').disabled=true;$('coworkAttachProjectSelection').disabled=true;
  try{
    const blob=await readCoworkBinary(path,entry.size);coworkProjectObjectUrl=URL.createObjectURL(blob);const host=$('coworkPreview');host.innerHTML='';
    if(mime==='application/pdf'||/\.pdf$/i.test(path)){const frame=document.createElement('iframe');frame.title='PDF предпросмотр';frame.src=coworkProjectObjectUrl;frame.setAttribute('sandbox','allow-same-origin');host.appendChild(frame);}
    else if(mime.startsWith('image/')){const img=document.createElement('img');img.alt=path;img.src=coworkProjectObjectUrl;host.appendChild(img);}
    else if(mime.startsWith('video/')){const video=document.createElement('video');video.controls=true;video.src=coworkProjectObjectUrl;host.appendChild(video);}
    else if(mime.startsWith('audio/')){const audio=document.createElement('audio');audio.controls=true;audio.src=coworkProjectObjectUrl;host.appendChild(audio);}
    else {const link=document.createElement('a');link.href=coworkProjectObjectUrl;link.download=path.split('/').pop();link.textContent=`Скачать ${path.split('/').pop()} для открытия в Office`;host.appendChild(link);const note=document.createElement('p');note.className='empty';note.textContent='Бинарные Office-файлы доступны агенту в рабочей папке, но встроенное редактирование DOCX/XLSX/PPTX требует отдельного Office renderer.';host.appendChild(note);}
  }catch(err){$('coworkPreview').innerHTML=`<p class="empty">${esc(err.message)}</p>`}
}
$('coworkRefreshFiles').addEventListener('click',()=>loadCoworkProjectFiles('').catch(e=>toast(e.message)));
$('coworkProjectFiles').addEventListener('click',e=>{const button=e.target.closest('[data-cowork-file]');if(button)openCoworkProjectFile(button.dataset.coworkFile).catch(err=>toast(err.message))});
document.querySelectorAll('[data-cowork-pane]').forEach(b=>b.addEventListener('click',()=>setCoworkPreviewPane(b.dataset.coworkPane)));
document.querySelector('[data-view-target="cowork"]')?.addEventListener('click',()=>loadCoworkProjectFiles('').catch(()=>{}));
$('coworkProjectEditor').addEventListener('input',()=>{coworkProjectDirty=$('coworkProjectEditor').value!==coworkProjectOriginal;$('coworkSaveProjectFile').disabled=!coworkProjectDirty;renderCoworkPreview()});
$('coworkSaveProjectFile').addEventListener('click',async()=>{if(!currentTaskId||!coworkProjectSelectedPath)return;try{await workspaceRequest('write',{path:coworkProjectSelectedPath,content:$('coworkProjectEditor').value},currentTaskId);coworkProjectOriginal=$('coworkProjectEditor').value;coworkProjectDirty=false;$('coworkSaveProjectFile').disabled=true;renderCoworkPreview();toast('Файл сохранён в рабочей папке задачи')}catch(err){toast(err.message)}});
$('coworkAttachProjectSelection').addEventListener('mousedown',e=>e.preventDefault());
$('coworkAttachProjectSelection').addEventListener('click',()=>{const selected=coworkCaptureSelection();if(!selected){toast('Выделите фрагмент в редакторе проекта');return}coworkPendingSelections.push({...selected,label:coworkProjectSelectedPath||selected.label,kind:'code'});$('coworkSelectionStatus').textContent=`Добавлено выделение: ${coworkPendingSelections.length}`;toast('Фрагмент добавлен к следующему отчёту')});

async function boot(){try{await ensureToken();if(!token)throw new Error('Токен доступа не задан');await Promise.all([loadAgents(),refresh(),loadModels(),loadConfigs(),loadResources(),loadPlan()]);activateView(sessionStorage.getItem('ao_active_view_v2')||'chat');const settingsSection=sessionStorage.getItem('ao_settings_section')||'nodes';document.querySelector(`[data-settings-target="${settingsSection}"]`)?.click()}catch(e){setConnection(false,'Требуется токен');if(!e.message.includes('Токен'))toast(e.message)}refreshTimer=setInterval(()=>{refresh().catch(()=>{});loadModels().catch(()=>{});loadPlan().catch(()=>{})},5000)}
window.addEventListener('beforeunload',event=>{
  if(refreshTimer)clearInterval(refreshTimer);
  if(workspaceDirty||workspaceSaving){event.preventDefault();event.returnValue='';}
});boot();
