
pipeline {
	agent none
	stages {
		stage ("NEMU build") {
			parallel {
				stage ("x86-64 build") {
					agent {
						label 'xenial'
					}
					stages {
						stage ('Checkout') {
							steps {
								checkout scm
							}
						}
						stage ('Prepare') {
							steps {
							sh "sudo apt-get update"
							sh "sudo apt-get build-dep -y qemu"
							}
						}
						stage ('Compile') {
							steps {
								sh "SRCDIR=$WORKSPACE tools/build_x86_64.sh"
							}
						}
						stage ('NATS') {
							steps {
								sh "SRCDIR=$WORKSPACE tools/CI/run_nats.sh"
							}
						}
					}
				}
				stage ("aarch64 build") {
					agent {
						label 'xenial-arm'
					}
					stages {
						stage ('Checkout') {
							steps {
								checkout scm
							}
						}
						stage ('Compile') {
							steps {
								sh "SRCDIR=$WORKSPACE tools/build_x86_64.sh"
							}
						}
					}
				}
			}
		}
	}
}

