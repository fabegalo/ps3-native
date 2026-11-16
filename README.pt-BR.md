<div align="center">

<img src="docs/assets/ps3-native-banner.png" alt="Arte conceitual do ps3-native com CELL, SPUs, RSX e execução nativa no PC; não é captura de jogo" width="100%" />

# ps3-native

**Projeto aberto de pesquisa para recompilação estática AOT de software do PlayStation 3, visando jogos originais e a VSH/XMB autêntica.**

[![CI](https://img.shields.io/github/actions/workflow/status/fabegalo/ps3-native/ci.yml?branch=main&label=CI)](https://github.com/fabegalo/ps3-native/actions/workflows/ci.yml)
[![Marco](https://img.shields.io/badge/milestone-M0%20%7C%20bootstrap-555)](#estado-atual)
[![Experimental](https://img.shields.io/badge/status-experimental-orange)](#estado-atual)
[![Licença](https://img.shields.io/badge/license-BSD--3--Clause-blue)](LICENSE)

[English](README.md) · [Português (Brasil)](README.pt-BR.md) · [Compilação](#compilar-e-verificar) · [Comandos](#contratos-dos-comandos)

</div>

> **Pesquisa experimental, ainda sem jogos funcionais.** O banner é uma arte conceitual. Os testes sintéticos PPU/SPU e RSX passam. O launcher agora entra no código PPU original de inicialização de um jogo retail em uma sessão AOT diagnóstica e limitada. Menu, gameplay e boot da VSH/XMB original não foram verificados.

Baseado no [ps3recomp](https://github.com/sp00nznet/ps3recomp), de **Ned Heller / sp00nznet**, e na arquitetura compartilhada de [x360-native](https://github.com/fabegalo/x360-native), [ps2-native](https://github.com/fabegalo/ps2-native) e [xone-native](https://github.com/fabegalo/xone-native). A meta é preservar o ciclo autêntico **sistema → jogo → sistema**; o gerenciador não imita a XMB.

<a id="estado-atual"></a>

## Estado atual

| Área | Já demonstrado | Ainda não demonstrado |
| --- | --- | --- |
| PPU / SPU AOT | Programas sintéticos próprios compilados e executados; DMA SPU conferida | Execução genérica de jogos comerciais |
| Gráficos RSX | Regressão upstream D3D12 de pixels/profundidade validada em GPU local | Títulos jogáveis ou ampla cobertura RSX |
| Importação / preparação | Inventário somente leitura de jogos e PUP; geração AOT limitada de ELF PPU | Binding completo de módulos e execução nativa genérica |
| Firmware original | Inspeção de identidade PUP e pesquisa LLE do upstream | Boot VSH/XMB original, navegação e retorno dos jogos |
| Plataformas | Windows x64 validado localmente; CI Windows, Linux e macOS configurada | Certificação de execução nos demais alvos |

## Compilar e verificar

Requisitos: Git, CMake 3.25+, Ninja, Python 3.11+ e Clang com suporte a C/C++20.
No Windows, use **clang-cl**, headers/bibliotecas MSVC e Windows SDK;
`cl.exe` não suporta os builtins usados pelo upstream. Reutilize ferramentas
instaladas. Linux e macOS precisam dos arquivos de desenvolvimento do SDL2.
A CI inicial inclui Windows, Linux e macOS; os alvos ARM continuam planejados.

```sh
git clone --recurse-submodules https://github.com/fabegalo/ps3-native.git
cd ps3-native
# Checkout existente: git submodule update --init
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
```

No Windows, em um prompt de desenvolvimento x64 com clang-cl, CMake e Ninja no PATH:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
.\build\ps3-native.exe --help
```

`scripts/devshell.cmd` inicializa o ambiente Visual Studio x64 já instalado;
não instala ferramentas nem altera o ambiente global. O launcher deste checkout
usa o Python e o código-fonte configurados pelo CMake. Ainda não é uma distribuição
portável para o usuário final. O M0 não cria ZIP ou Release.

## Contratos dos comandos

No POSIX, o launcher é `build/ps3-native`. O mesmo CLI também aceita:

```sh
python src/ps3native/cli.py inspect "/caminho/jogo/PS3_GAME"
python src/ps3native/cli.py --catalog .local/catalog import "/caminho/PS3UPDAT.PUP"
python src/ps3native/cli.py prepare "/caminho/legivel/EBOOT.elf"
python src/ps3native/cli.py prepare --fixture ppu
python src/ps3native/cli.py run --fixture ppu
python src/ps3native/cli.py prepare --fixture spu
python src/ps3native/cli.py run --fixture spu
python src/ps3native/cli.py system
```

Organização local sugerida: `content/games/<jogo>/PS3_GAME/` e
`firmware/PS3UPDAT.PUP`, ambos ignorados pelo Git. Os originais também podem
continuar em outro disco; importe o caminho existente sem movê-los ou duplicá-los.

| Comando | Comportamento efetivo no M0 |
| --- | --- |
| `inspect CAMINHO` | Inventário recursivo somente leitura: tamanhos, SHA-256, PARAM.SFO, tabela/versão textual do PUP e identificação ELF/SELF/PKG. Saída JSON. |
| `import CAMINHO` | Mesmo inventário, com manifesto publicado atomicamente fora dos originais. Referencia caminhos, sem copiar árvores de jogos; importações iguais reutilizam o ID. |
| `prepare CAMINHO [--cache CAMINHO]` | Um ELF64 PPU ET_EXEC big-endian legível: parsing, OPD/TOC/imports e geração AOT C pelo upstream. Recibos verificam hashes da entrada, adaptador, upstream e saídas. Retorna **3**, pois a compilação e certificação genérica de dependências continuam bloqueadas. |
| `prepare --fixture ppu\|spu [--build-dir CAMINHO]` | Gera, compila e vincula a fixture explicitamente sintética ao runtime existente. CMake/Ninja controlam invalidação incremental por compilador, headers, ferramentas e entradas. |
| `run --fixture ppu\|spu` | Confere upstream e hashes da fixture, executa o binário sintético e devolve seu resultado real. |
| `run CAMINHO` | Inicia o módulo nativo diagnóstico vinculado por hashes àquela revisão, ou retorna **3** se não houver módulo preparado. Suporte ausente interrompe com **86**. |
| `system` | Retorna **3**: boot VSH/XMB original e ciclo sistema/jogo ainda não implementados. |

### Diagnóstico de inicialização nativa

Forneça um ELF legível obtido localmente do seu original. A ferramenta portátil
oficial `RPCS3 --decrypt` pode obtê-lo; RPCS3 não é o backend de execução.
Não coloque ELF, chaves ou código gerado no Git. Gere AOT com `prepare EBOOT.elf`;
o JSON informa o cache mesmo retornando 3, pois a compilação ainda está pendente.
Depois:

```sh
python src/ps3native/cli.py prepare /original/PS3_GAME --build-native --elf /local/EBOOT.elf --aot-dir /local/cache-aot
python src/ps3native/cli.py run /original/PS3_GAME
```

A fatia offline inclui a entrada, pequenas tabelas OPD e destinos diretos.
Quando uma execução parar por destino PPU ausente, repita a preparação para
ampliá-la e recompilar incrementalmente. Não há compilação durante execução
guest, intérprete ou fallback JIT. HLE exige implementação auditada;
instruções/chamadas sem suporte interrompem com 86. A sessão confere hashes dos
originais, ELF, código gerado, binário, runtime e configuração, separa o HDD
gravável e para após 20 segundos ou quando os logs ultrapassam 1 MiB.
Início diagnóstico não comprova gameplay.

Códigos: **0** operação concluída; **2** entrada inválida/ausente, erro de I/O
ou ferramenta; **3** etapa de preparação/execução indisponível. Preparação e
execução das fixtures preservam os códigos reais do compilador/processo.
O ID considera nomes relativos, tamanhos, tipos e hashes; não autentica firmware.
Links/junctions e limites SFO/PUP inválidos são rejeitados. Reutilizar cache
exige conferir todos os hashes de saída; mudanças de entrada/adaptador geram
outro cache. Uma etapa interrompida não substitui caches válidos. A geração
copia temporariamente apenas um executável, limitado a 256 MiB, sem duplicar
jogos. Mantenha os originais estáveis durante a inspeção; não há snapshot
transacional do sistema de arquivos ou sandbox de segurança.

## Arquitetura e dependência

As sete fronteiras seguem o
[x360-native](https://github.com/fabegalo/x360-native), com
[ps2-native](https://github.com/fabegalo/ps2-native) e
[xone-native](https://github.com/fabegalo/xone-native) como referências:
launcher; conteúdo/catálogo; preparação/execução nativa; runtime/HLE;
Original GPU/Native GPU; sistema/jogos originais; supervisor/adaptadores.
Responsabilidades atuais e validação de novas decisões estão em [AGENTS.md](AGENTS.md).

O submódulo preservado e `deps.lock.json` fixam o ps3recomp em
**`de4f090328ea08bef6b2f15caec0b85e343a7ece`**, HEAD consultado em 14/11/2026.
Um patch genérico com hashes fixos bloqueia destinos PPU diretos/indiretos
ausentes, LV2 sem implementação e interpretação SPU; restringe HLE a serviços
auditados e preserva callbacks registrados de saída/spawn; usa contabilidade
real de memória e horário UTC correto no Windows. Não há runtime
duplicado. Registradores, VMX, endianness,
OPD/TOC e memória PPU, local store, DMA e canais SPU vêm do upstream. As fixtures
não certificam toda a ISA, exceções, SPURS, sincronização ou ponto flutuante.
O upstream contém intérprete e caminhos específicos por título; sua existência
não os autoriza como fallback de produção do ps3-native.

| Backend | Disponibilidade upstream / evidência |
| --- | --- |
| D3D12 | Windows; regressão de pixels validada localmente na NVIDIA RTX 4070. |
| Vulkan | Opcional somente no Linux: `-DPS3RECOMP_RSX_VULKAN=ON`; precisa de headers Vulkan; shaders guest precisam de glslang. Sem validação local. |
| Metal | Somente Apple; tradução de shaders guest precisa de glslang + spirv-cross. Sem validação local. |

Fidelidade Original e melhorias Native devem compartilhar contratos RSX e estado
coerente entre PPU/SPU/RSX. O M0 não cria renderer nem implementa melhorias Native.
A CI hospedada exclui execução do teste D3D12 porque não possui adaptador certificado;
o teste permanece obrigatório em uma máquina local com GPU apropriada.

## Validação e próximo bloqueio real

Windows x64 Release local, Clang 22.1.8, CMake 3.31.6 e Python 3.13.17:

- PPU: ELF PPC64 próprio → análise OPD/lifter upstream → compilação nativa →
  loader/dispatch upstream → resultado big-endian **42**. Controle com resultado
  esperado incorreto falha.
- SPU: instruções próprias → lifter upstream → execução nativa → MFC GET,
  incremento de quatro palavras e PUT; **16 bytes** conferidos em LS e memória
  principal. Controle com entrada corrompida falha. O teste linear não usa intérprete.
- RSX: comandos NV4097 e programas vertex/fragment do teste upstream → D3D12
  na GPU física → leitura de pixels. Quatro casos de profundidade Z16/Z24S8,
  **256 pixels por caso**: três desenhos brancos e um desenho ocluído preto.
- Catálogo/preparação: limites SFO/PUP, Unicode, idempotência, originais,
  revisões, alteração real do código gerado, cache e adulteração, comandos indisponíveis.
  O caso de symlink é pulado explicitamente quando o host não permite criar links.
- Runtime: chamadas ausentes/não auditadas e interpretação SPU interrompem;
  exclusão mútua real, callbacks, alocação/liberação e relógios são conferidos
  com contextos sintéticos.

As regressões públicas usam fixtures próprias. Um jogo retail fornecido pelo
usuário também foi testado localmente até o código original de inicialização
do CRT; código e resultados derivados permanecem ignorados pelo Git.
Nenhum SDK Sony foi usado. A compilação validada
localmente é Windows x64; Linux/macOS dependem da execução CI do commit exato.
Windows/Linux ARM64, variantes Intel/Apple Silicon e Steam Deck exigem evidência
própria; uma matriz de runners não comprova todas as arquiteturas ou execução.

A sessão local de **EL CHAVO KART (BLUS31198, 01.00)** chega ao CRT/construtores
PPU originais, metadados SFO e pool NP local sem login. Ela inicializa GCM, abre
a saída D3D12 real em 1280×720, mapeia 10 MiB de memória principal no IO RSX,
registra três framebuffers e executa uma fence por label e um comando de limpeza
de tela no FIFO RSX. O código original cria as threads `FlipPump` e
`_SPU_printf_server`, uma fila de eventos e uma porta conectada.
A `libsre.prx` original do firmware 4.93, autorizada pelo mantenedor, agora
fornece `cellSpurs`: 440 funções PPU e 149 exports compilados e separados do HLE.
Os inicializadores originais criam um grupo exclusivo com cinco SPUs e um grupo
normal com um worker. Os seis workers nativos entram no código SPU original do
firmware; o kernel chega a `0x13F4`. O join espera a conclusão real, e regressões
sintéticas de mailbox/reinício conferem os status de saída 42 e 17. STOP terminal
encerra a execução SPU pertencente ao engine, sem retomar os chamadores traduzidos.
Código transferido por DMA é preparado offline, identificado pelo conteúdo completo
carregado e conferido byte a byte na entrada/retorno das funções. Escritas retiram
código antigo de circulação; alterações de código ativo e destinos desconhecidos param.

Relatórios ZPASS e timestamps reais da GPU D3D12 agora respeitam a ordem dos comandos
sem apresentação antecipada. O teste na GPU física confere 4096 amostras visíveis,
zero amostras ocluídas e 256 após limpeza parcial de profundidade. Flips preparados
retornam os índices originais da fila e aguardam a submissão GPU. O backend atual
para explicitamente se houver mais de um quadro preparado em voo, até que as
superfícies de exibição sejam preservadas separadamente. Uma janela aberta ainda
não comprova um frame do jogo ou menu. El Chavo agora imprime
`FIOS 1.3.5 initialized` e lê português do Brasil do idioma da interface do host.
A instalação usa uma área temporária limitada no HLE: copia e sincroniza o ícone
original de 96.738 bytes e publica o diretório atomicamente com um SFO real.
O SHA-256 do ícone copiado coincide com o original. O boot seguinte preserva e
reconhece essa instalação. O estudo offline da interface de jogo do
`basic_plugins` original confirmou `sysSizeKB=3` para GAMEDATA; o tamanho de
conteúdo ainda não calculado permanece NOTCALC. O boot agora para em
`sceNpTrophyCreateContext`, cuja gestão de contexto/assinatura ainda exige auditoria.
Frames originais continuam sem verificação.
O TTY preserva bytes binários e quantidades reais. A consulta somente de leitura
valida caminhos, categoria SFO e buffers; diretórios vazios não são instalações.
Escritas ficam restritas à instalação ativa; links, substituição de metadados
e excesso do orçamento diagnóstico param explicitamente. Chamadas LV2 e HLE
compartilham descritores, contagens e escritas coerentes na memória guest.
O idioma usa valores suportados da configuração/host; outras preferências e
idiomas sem mapeamento param explicitamente.

As 47 imagens SPU ELF descobertas no jogo, duas ELF embutidas no firmware e dois
payloads brutos verificados estão compilados localmente: 51 registros com hashes
de entrada/ferramentas/saídas. Limites explícitos separam código original de dados
constantes em quatro payloads; todos os dados carregados são preservados. Isso
comprova preparação, sem certificar fluxo completo, ISA, frames ou gameplay.

O novo **God of War Collection (BCUS98229, 02.00)** tem catálogo somente de leitura
e quatro ELF PPU extraídos localmente e validados estruturalmente. O EBOOT inicial
tem 9.951 funções PPU compiladas localmente e 13 programas SPU mais duas variantes
de código AOT verificadas preparadas,
incluindo o firmware SPURS original. A `libnet.prx` original autorizada também
está vinculada por AOT: 358 funções PPU, 100 exports e a entrada original de início.
O boot passa pelo caminho de inicialização dessa biblioteca e lê
`defaultSettings.cxml`. Reconhecer as pilhas realmente alocadas para callbacks
corrige o EFAULT que impedia a inicialização do SPURS e da biblioteca de som.
Os seis workers SPURS originais agora iniciam; o mixer estéreo do host inicia
e uma porta guest de oito canais é aberta. O kernel WWS original agora trata
interrupções DMA usando suas rotinas de preservação/restauração em LS 0x3FEC0.
As variantes compiladas offline conferem os 320 bytes reais e o vínculo ao dono
residente a cada chamada, inclusive com cache ativo; bytes desconhecidos param.
A preparação compartilhada também compila entradas de retorno de chamadas para
retomar tarefas SPU. A porta de oito canais inicia com mistura estéreo verificada
e consumo real no host. SCREAM inicializa e o boot lê/descomprime dados. A consulta
de controles informa conexões e recursos medidos no host, sem porta implícita
nem suporte inventado a pressão/sensores. A configuração de portas preserva os
modos solicitados separadamente das capacidades medidas, inclusive sem controle conectado.
A `libgcm_sys` original agora é compilada, vinculada e iniciada por AOT
(262 funções PPU, 102 exports). Sua ponte LV2 fornece alocações RSX com respaldo,
fila real de interrupções, mapeamentos IO, labels/relatórios compartilhados,
controle FIFO, tiles sem compressão e buffers de imagem. O boot em VSYNC abre a janela D3D12.
Aquisições de semáforo aguardam; liberações de backend exigem conclusão real da GPU.
A sessão mais recente para em `cellPadGetData`; a entrega de pacotes continua
bloqueada até sua auditoria. O caminho original de flip/interrupções ainda não
foi verificado. Use `run ... --enter-button cross`
ou `circle` explicitamente; idioma/região não inferem a preferência. Não há interpretação.
Nenhum frame, transição para o seletor ou gameplay foi verificado.
Originais e derivados proprietários permanecem fora do Git.
Criação/destruição de mutexes do kernel, locks sem disputa, recursão e erros de
try-lock têm regressões sintéticas; esperas disputadas param explicitamente até
implementar a ordem FIFO/prioridade dos waiters guest.
Disponibilidade de módulo e execução de cada API são verificadas separadamente.
Layouts de atributos/vídeo, quota e backing do mapper, esperas/callbacks e
metadados de boot em disco têm regressões sintéticas; tiles comprimidos param.
Criação/saída/join de threads PPU, consulta de prioridade e limites SPU também
têm regressões. Esperas Windows conferem o prazo monotônico após o timer do host.
Inicializar recursos não habilita execução SPU ainda não preparada.
Publicação de inicializadores únicos, criação/conexão de eventos, flags SPURS
e registro de callbacks printf têm regressões. Flips preparados aguardam seu
consumo no FIFO; callbacks antecipados e descarte de frames para alcançar a fila
ficam desativados na execução estrita. Paradas fatais dispensam o teardown guest.

**Próxima tarefa:** concluir CRT/construtores originais através das paradas
diagnósticas, ampliar destinos indiretos preparados e auditar serviços alcançados
com regressões reais. Continuar a inicialização original e verificar um frame real do jogo antes de
anunciar suporte a menu ou gameplay. A lista HLE auditada e as barreiras param onde o suporte termina;
completude de produção e ABI ampla continuam sem comprovação. Hooks upstream
por título não provam compatibilidade genérica.

Imagens/overlays SPU dinâmicos precisam de fingerprint, endereço/faixa LS,
revisão de runtime/lifter e identidade de relocação, preparação AOT fora da
execução ativa e invalidação por DMA/escrita de código. Imagens desconhecidas
devem bloquear com diagnóstico útil. M0 não autoriza fallback JIT/intérprete.

## Preparação do firmware original / XMB

A importação PUP registra identidade, entradas com limites conferidos e versão
legível (entrada 0x100). Não verifica assinatura/HMAC Sony, descriptografa,
extrai dev_flash, carrega PRX ou executa VSH. SELF criptografado, ELF legível,
saída AOT, módulo compilado e sessão executável são estados diferentes.

[FIRMWARE_LLE.md](external/ps3recomp/docs/FIRMWARE_LLE.md) documenta
`tools/lift_prx.py`: PRX fornecido pelo usuário **já descriptografado** → imagem
relocada, funções, imports/exports → `ppu_lifter.py --raw --base` → módulo
compilado. O exemplo SPURS não comprova boot da XMB. Essas saídas devem ficar
locais, ignoradas pelo Git e separadas do HLE, com hashes originais, revisão do
firmware, base/relocações e propriedade explícita de NIDs. Não redistribuí-las.

`src/ps3native/firmware.py` valida um PRX já descriptografado, usa as ferramentas
upstream fixadas de relocação/lifting e gera um recibo AOT vinculado por hashes.
Informe a pasta desse recibo em `PS3_NATIVE_FIRMWARE_DIR` na configuração CMake
existente. Apenas a biblioteca escolhida fornece os exports; exports originais
ausentes não caem silenciosamente no HLE. Módulos que exigem inicialização por
module-start são rejeitados. Preparação e runtime rejeitam sobreposições e áreas
reservadas de scratch/TLS. Uma fixture PRX própria verifica stack/TOC/TLS, cache,
adulteração de exports e rejeição de fallback. A extração/descriptografia local
usa RPCS3 somente como ferramenta offline; a execução guest usa ps3recomp.
Escritas em bloco e strings invalidam reservas PPU/SPU existentes. Imports SPU
diretos validam ELF e publicam registros COPY/FILL/INFO reais; imports protegidos
continuam bloqueados. A recepção espera eventos reais e retorna registradores r4–r7.

A VSH/XMB também exige extração autenticada do firmware, grafo completo de
módulos e contratos do loader, dev_flash/fontes/ícones, serviços LV2,
usuários/armazenamento/saves, controles, biblioteca e inicialização/retorno
supervisionado de jogos. Essas dependências e a barreira de execução genérica
impedem a XMB hoje. Extração ISO/PKG, DLC/dados instalados e distribuição com
GUI ficam para etapas seguintes; identificar contêiner não significa executá-lo.

## Licenças

Código próprio: [BSD-3-Clause](LICENSE). ps3recomp: MIT, com licenças adicionais,
incluindo o decoder ATRAC LGPL-2.1 habilitado por padrão.
Veja [os créditos de terceiros](THIRD_PARTY_NOTICES.md).
[RPCS3](https://github.com/RPCS3/rpcs3) é referência técnica/comportamental;
sua implementação não é copiada. Forneça originais legítimos próprios; não
distribuímos jogos, firmware, chaves, SDK ou código proprietário gerado.
