parallel ('xenial': {
	node ('xenial') {
		stage ('Checkout: x86-64') {
			checkout scm
		}
		stage ('Prepare: x86-64') {
			sh "sudo apt-get update"
			sh "sudo apt-get build-dep -y qemu"
		}
		stage ('Compile: x86-64') {
			sh "SRCDIR=$WORKSPACE tools/build_x86_64.sh"
		}
		stage ('NATS: x86-64') {
			sh "SRCDIR=$WORKSPACE tools/CI/run_nats.sh"
		}
	}
}, 'xenial-arm': {
		node ('xenial-arm') {
		stage ('Checkout: aarch64') {
			checkout scm
		}
		stage ('Compile: aarch64') {
			sh "SRCDIR=$WORKSPACE tools/build_x86_64.sh"
		}
	}
})

