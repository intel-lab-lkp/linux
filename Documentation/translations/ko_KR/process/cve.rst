.. SPDX-License-Identifier: GPL-2.0

NOTE:
This is a version of Documentation/process/cve.rst translated into Korean.
This document is maintained by Yunseong Kim <ysk@kzalloc.com>.
If you find any difference between this document and the original file or
a problem with the translation, please contact the maintainer of this file.

Please also note that the purpose of this file is to be easier to
read for non English (read: Korean) speakers and is not intended as
a fork.  So if you have any comments or updates for this file please
update the original English file first.  The English version is
definitive, and readers should look there if they have any doubt.

================================================================
이 문서는
Documentation/process/cve.rst
의 한글 번역입니다.

:역자: 김윤성 <ysk@kzalloc.com>
:감수: 박성재 <sj@kernel.org>, 김동현 <austindh.kim@gmail.com>,
       박진우 <pmnxis@gmail.com>
================================================================

=================
CVE 항목들 (CVEs)
=================

공통 취약점 및 노출(Common Vulnerabilities and Exposure, CVE®) 번호는 공개적으로
알려진 보안 취약점을 식별, 정의하고 목록화하기 위한 명확한 방법으로
개발되었습니다. 하지만 시간이 지나면서 커널 프로젝트에서는 그 유용성이
감소했으며, CVE 번호가 부적절한 방식과 이유로 할당되는 경우가 매우 많았습니다.
이 때문에 커널 개발 커뮤니티는 CVE 사용을 꺼리는 경향이 있었습니다. 그러나
CVE 및 기타 보안 식별자 할당에 대한 지속적인 압력과, 커널 커뮤니티 외부의
개인 및 회사들의 지속적인 남용이 결합되면서, 커널 커뮤니티가 이러한 할당에
대한 통제권을 가져야 한다는 점이 명확해졌습니다.

리눅스 커널 개발팀은 잠재적인 리눅스 커널 보안 이슈에 대해 CVE를 할당할 수
있습니다. 이 할당은 :doc:`일반적인 리눅스 커널 보안 버그 보고
절차<Documentation/process/security-bugs>`와는 독립적으로 이루어집니다.

리눅스 커널에 할당된 모든 CVE 목록은 linux-cve 메일링 리스트 아카이브
(https://lore.kernel.org/linux-cve-announce/)에서 확인할 수 있습니다.
할당된 CVE에 대한 알림을 받으려면 해당 메일링 리스트를
`구독<https://subspace.kernel.org/subscribing.html>`_ 하시기 바랍니다.

프로세스 (Process)
==================

일반적인 안정(stable) 릴리스 프로세스의 일부로, 잠재적으로 보안 이슈가 될 수
있는 커널 변경 사항은 CVE 번호 할당 담당 개발자가 식별하여 자동으로 CVE 번호가
할당됩니다. 이러한 할당 내역은 linux-cve-announce 메일링 리스트에 공지사항으로
빈번하게 게시됩니다.

참고로, 리눅스 커널이 시스템에서 차지하는 계층의 특성상 거의 모든 버그가 커널
보안을 침해하는 데 악용될 수 있지만, 버그가 수정될 당시에는 악용 가능성이
명확하지 않은 경우가 많습니다. 이 때문에 CVE 할당 팀은 매우 신중하게
접근하며(overly cautious), 식별한 모든 버그 수정(bugfix)에 CVE 번호를
할당합니다. 이는 리눅스 커널 팀이 발행하는 CVE의 수가 겉보기에 많아 보이는
이유를 설명합니다.

만약 CVE 할당 팀이 놓친 특정 수정 사항에 대해 CVE가 할당되어야 한다고 생각되면,
<cve@kernel.org>로 이메일을 보내주십시오. 담당 팀이 협력할 것입니다.
이 이메일 주소는 이미 릴리스된 커널 트리에 포함된 수정 사항에 대한 CVE 할당
전용이며, 잠재적인 보안 이슈를 보내서는 안 된다는 점에 유의하십시오.
아직 수정되지 않은 보안 이슈를 발견했다고 생각되면 :doc:`일반적인 리눅스
커널 보안 버그 보고 절차<Documentation/process/security-bugs>` 를 따르십시오.

리눅스 커널의 수정되지 않은 보안 이슈에 대해서는 CVE가 자동으로 할당되지
않습니다. 할당은 수정 사항이 제공되고 안정(stable) 커널 트리에 적용된 후에만
자동으로 이루어지며, 원본 수정 사항의 git 커밋 ID로 추적됩니다. 커밋으로
이슈가 해결되기 전에 CVE를 할당받고자 하는 경우, 커널 CVE 할당
팀(<cve@kernel.org>)에 연락하여 예약된 식별자 목록에서 할당받을 수 있습니다.

현재 안정/장기 지원 버전(Stable/LTS) 커널 팀이 적극적으로 지원하지 않는 커널
버전에서 발견된 이슈에 대해서는 CVE가 할당되지 않습니다. 현재 지원되는 커널
브랜치 목록은 https://kernel.org/releases.html 에서 확인할 수 있습니다.

할당된 CVE에 대한 이의 제기 (Disputes)
======================================

특정 커널 변경 사항에 할당된 CVE에 대해 이의를 제기하거나 수정할 권한은 오직
영향을 받는 관련 서브시스템의 메인테이너에게만 있습니다. 이 원칙은 취약점
보고의 높은 정확성과 책임성을 보장합니다. 서브시스템에 대한 깊은 전문 지식과
긴밀한 이해(intimate knowledge)를 가진 개인만이 보고된 취약점의 유효성과
범위를 효과적으로 평가하고 적절한 CVE 지정을 결정할 수 있습니다. 이 지정된
권한 밖에서 CVE를 수정하거나 이의를 제기하려는 시도는 혼란, 부정확한 보고,
그리고 궁극적으로 시스템 침해로 이어질 수 있습니다.

유효하지 않은 CVE (Invalid CVEs)
================================

리눅스 배포판이 자체적으로 적용한 변경 사항 때문에 해당 배포판에서만 지원되는
리눅스 커널에서 보안 이슈가 발견된 경우, 또는 배포판이 더 이상 kernel.org에서
지원하는 릴리스가 아닌 커널 버전을 지원하여 보안 이슈가 발견된 경우, 리눅스
커널 CVE 팀은 CVE를 할당할 수 없으며 해당 리눅스 배포판에 직접 요청해야
합니다.

적극적으로 지원되는 커널 버전에 대해, 커널 CVE 할당 팀이 아닌 다른 그룹이
리눅스 커널에 할당한 모든 CVE는 유효한 CVE로 간주되어서는 안 됩니다.
커널 CVE 할당 팀(<cve@kernel.org>)에 알려주시면 CNA(CVE Numbering Authority)
시정(remediation) 절차를 통해 해당 항목을 무효화하도록 조치할 것입니다.

특정 CVE의 적용 가능성 (Applicability)
======================================

리눅스 커널은 다양한 방식으로 사용될 수 있으며, 외부 사용자가 접근하는 방식도
다양하거나 아예 접근이 없을 수도 있습니다. 따라서 특정 CVE의 적용 가능성(해당
여부)은 CVE 할당 팀이 아닌 리눅스 사용자가 결정해야 합니다. 특정 CVE의 적용
가능성을 판단하기 위해 저희에게 연락하지 마십시오.

또한, 소스 트리는 매우 방대하고 개별 시스템은 소스 트리의 작은 부분 집합만을
사용하므로, 리눅스 사용자는 할당된 수많은 CVE가 자신의 시스템과 관련이 없다는
점을 인지해야 합니다.

요컨대, 저희는 귀하의 사용 사례(use case)를 알지 못하며 귀하가 커널의 어느
부분을 사용하는지 알지 못하므로, 특정 CVE가 귀하의 시스템과 관련이 있는지
판단할 방법이 없습니다.

언제나 그렇듯이, 개별적으로 선별된(체리픽된) 변경 사항이 아니라, 많은 커뮤니티
구성원들에 의해 통합된 전체로서 함께 테스트된 모든 릴리스된 커널 변경 사항을
적용하는 것이 가장 좋습니다. 또한 많은 버그의 경우, 전체 문제에 대한 해결책은
단일 변경 사항이 아니라 서로 중첩된 많은 수정 사항의 합으로 발견된다는 점에
유의하십시오. 이상적으로는 모든 이슈에 대한 모든 수정 사항에 CVE가 할당되지만,
때때로 저희가 수정 사항을 인지하지 못할 수 있습니다. 따라서 CVE가 할당되지
않은 일부 변경 사항도 적용하는 것이 관련 있을 수 있다고 가정하십시오.